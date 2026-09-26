#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

#include <unordered_set>

namespace renderer_state_validation
{
	namespace
	{
		std::uintptr_t fallback_state_address{};
		std::unordered_set<std::uint64_t> reported_states{};

		void report_invalid_state(const int path, const char* name, const std::uint32_t encoded_index,
			const std::uintptr_t owner)
		{
			char safe_name[128] = "(null)";
			if (name)
			{
				__try
				{
					strncpy_s(safe_name, name, _TRUNCATE);
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					strcpy_s(safe_name, "<invalid pointer>");
				}
			}

			const auto state_index = encoded_index / 3;
			const auto key = (static_cast<std::uint64_t>(path) << 32) | state_index;
			if (!reported_states.insert(key).second)
			{
				return;
			}

			const auto fallback = *reinterpret_cast<const void**>(fallback_state_address);
			console::error(
				"[renderer] invalid state slot %u in R_SetState path %d: name='%s', owner=0x%08X; using fallback state 0x%08X\n",
				state_index, path, safe_name, owner, reinterpret_cast<std::uintptr_t>(fallback));
		}

		__declspec(naked) void set_pass_state_fallback_stub()
		{
			__asm
			{
				pushad
				mov eax, [esp + 0x34]
				mov ecx, [esp + 0x1C]
				mov edx, [esp + 0x08]
				push edx
				push ecx
				push eax
				push 0
				call report_invalid_state
				add esp, 0x10
				popad

				mov eax, fallback_state_address
				mov esi, [eax]
				ret
			}
		}

		__declspec(naked) void set_primitive_state_fallback_stub()
		{
			__asm
			{
				pushad
				mov eax, [esp + 0x34]
				mov ecx, [esp + 0x14]
				mov edx, [esp + 0x04]
				push edx
				push ecx
				push eax
				push 1
				call report_invalid_state
				add esp, 0x10
				popad

				mov eax, fallback_state_address
				mov ebp, [eax]
				ret
			}
		}

		__declspec(naked) void validate_state_fallback_stub()
		{
			__asm
			{
				pushad
				mov eax, [esp + 0x34]
				mov ecx, [esp + 0x1C]
				mov edx, [esp + 0x18]
				push edx
				push ecx
				push eax
				push 2
				call report_invalid_state
				add esp, 0x10
				popad
				ret
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// QoS PC r_state.cpp:1986 treats a missing renderer-state pointer as
			// fatal. Xenon material conversion can currently expose an invalid slot;
			// preserve the frame with the engine's own fallback state and report the
			// exact slot once so the converter can be corrected from runtime evidence.
			fallback_state_address = game::game_offset(0x10C4A2C8);
			utils::hook::call(game::game_offset(0x103813EC), set_pass_state_fallback_stub);
			utils::hook::call(game::game_offset(0x10381992), set_primitive_state_fallback_stub);
			utils::hook::call(game::game_offset(0x10384C6C), validate_state_fallback_stub);
			scheduler::on_shutdown([] { reported_states.clear(); });
		}
	};
}

REGISTER_COMPONENT(renderer_state_validation::component)
