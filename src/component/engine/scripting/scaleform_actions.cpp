#include <std_include.hpp>

#include "loader/component_loader.hpp"
#include "component/engine/console/command.hpp"
#include "game/game.hpp"

#include <utils/hook.hpp>

namespace scaleform_actions
{
	namespace
	{
		utils::hook::detour action_dispatch_hook;
		using action_dispatch_t = void(__stdcall*)(int, const char*, const char*);
		using is_wow64_process_2_t = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

		bool is_x86_on_arm64()
		{
			const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
			const auto is_wow64_process_2 = reinterpret_cast<is_wow64_process_2_t>(
				GetProcAddress(kernel32, "IsWow64Process2"));
			if (!is_wow64_process_2)
			{
				return false;
			}

			USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
			USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
			return is_wow64_process_2(GetCurrentProcess(), &process_machine, &native_machine)
				&& process_machine == IMAGE_FILE_MACHINE_I386
				&& native_machine == IMAGE_FILE_MACHINE_ARM64;
		}

		struct action_binding
		{
			const char* action_name;
			const char* native_command;
		};

		// "open_server_browser"/"openmenu_serverbrowser" were guesses made
		// before any real .menu existed to open, never confirmed against an
		// actual button click. Static analysis of jb_mp_s.dll cannot recover
		// the real action strings the compiled Flash movie sends -- they
		// live entirely in its ActionScript, not in this binary (confirmed:
		// no "singleplayer"/"guide"/"open_server_browser"-like literals exist
		// natively). Do not add more guesses here. Instead: launch the game,
		// click the button in question, and read its real action string from
		// the passthrough log below, then add the binding.
		constexpr action_binding known_bindings[] = {
			{"open_server_browser", "openmenu serverbrowser"},
			{"openmenu_serverbrowser", "openmenu serverbrowser"},
		};

		bool handle_action(const char* action)
		{
			if (!action)
			{
				return false;
			}

			for (const auto& binding : known_bindings)
			{
				if (_stricmp(action, binding.action_name) == 0)
				{
					command::execute(binding.native_command);
					game::Com_Printf(13, "[scaleform - actions] '%s' -> %s\n", action, binding.native_command);
					return true;
				}
			}

			// Every unmatched action passes through to the native menu system
			// untouched, but is logged first. This is how to find the real
			// action string for Single Player / Guide / any other button:
			// click it in-game and read this line from the console/log.
			game::Com_Printf(13, "[scaleform - actions] passthrough: '%s'\n", action);
			return false;
		}

		// QoS 0x10002280 reads the action from [ebp+0Ch] and returns with
		// retn 0Ch. EDI is populated by its prologue, not by the caller.
		void __stdcall action_dispatch_stub(int context, const char* action, const char* argument)
		{
			if (!handle_action(action))
			{
				reinterpret_cast<action_dispatch_t>(action_dispatch_hook.get_original())(context, action, argument);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// MinHook's generated relay is not a valid CFG target under ARM64's
			// x86 emulation. This optional diagnostic hook must not block startup.
			if (is_x86_on_arm64())
			{
				game::Com_Printf(13, "^3[scaleform - actions] skipped: unsupported under x86-on-ARM64 emulation\n");
				return;
			}

			const auto target = game::game_offset(0x10002280);
			if (utils::hook::is_relatively_far(reinterpret_cast<const void*>(target),
				reinterpret_cast<const void*>(action_dispatch_stub)))
			{
				game::Com_Printf(13, "^3[scaleform - actions] skipped: callback is outside rel32 range\n");
				return;
			}

			action_dispatch_hook.create(target, action_dispatch_stub);
			game::Com_Printf(13, "[scaleform - actions] installed: action dispatch logging + server browser binding\n");
		}
	};
}

REGISTER_COMPONENT(scaleform_actions::component);
