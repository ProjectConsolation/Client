#pragma once
#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>
#include <cstring>
#include "hud_call_abi.hpp"

namespace killcam_hud
{
	namespace
	{
		bool in_killcam()
		{
			// QoS 1.1 InKillcam menu expression (102C9E60), also consumed by
			// the follow message and killcam ragdolls. 1044AA00 updates it from
			// nextSnap's replay deltaTime; --> KisakCOD CG_SetNextSnap.
			return *reinterpret_cast<const int*>(game::game_offset(0x12A575A8)) != 0;
		}

		bool should_draw_gameplay_hud()
		{
			if (in_killcam()) return false;
			return reinterpret_cast<bool(*)()>(game::game_offset(0x102A8ED0))();
		}

		std::size_t draw_foreground_and_chat(const int foreground, const int local_client)
		{
			const auto result = abi::draw_foreground(reinterpret_cast<void*>(
				game::game_offset(0x102BC580)), local_client, foreground);
			if (in_killcam())
			{
				// CG_Draw2D's two existing chat calls are skipped with drawHud=false.
				// Draw the native message buffer once, AFTER the replay bars. Do not
				// enable gameplay HUD, change message lifetime, or duplicate live chat.
				reinterpret_cast<void(*)(int)>(game::game_offset(0x10284870))(local_client);
			}
			return result;
		}

		__declspec(naked) void foreground_entry()
		{
			// Call site 102857F4: foreground on stack, localClientNum in ESI.
			__asm
			{
				push esi
				push dword ptr [esp + 8]
				call draw_foreground_and_chat
				add esp, 8
				ret
			}
		}

		void migrate_stock_chat_bindings()
		{
			static bool queued = false;
			if (queued) return;
			// QoS native key records: 12 bytes, binding pointer at +8. Validated
			// against CL_GetKeyBinding and the existing controller bind bridge.
			const auto binding = [](const int key)
			{
				return *reinterpret_cast<const char* const*>(
					game::game_offset(0x11263624 + key * 12));
			};
			const auto* all = binding('t');
			const auto* team = binding('y');

			// Wait for profile/config loading; preserve custom and unbound keys.
			if (!all || !team || _stricmp(all, "chatmodeteam")
				|| _stricmp(team, "chatmodepublic")) return;
			queued = true;
			game::Cbuf_AddText(0, "bind t chatmodepublic\nbind y chatmodeteam\n");
		}
	}

	inline void install()
	{
		const auto gate = game::game_offset(0x10285738);
		const auto foreground = game::game_offset(0x102857F4);
		if (std::memcmp(reinterpret_cast<const void*>(gate), "\xE8\x93\x37\x02\x00", 5)
			|| std::memcmp(reinterpret_cast<const void*>(foreground), "\xE8\x87\x6D\x03\x00", 5))
		{
			console::error("[killcam HUD] skipped: unsupported CG_Draw2D call sites\n");
			return;
		}
		utils::hook::call(gate, should_draw_gameplay_hud);
		utils::hook::call(foreground, foreground_entry);
		scheduler::loop(migrate_stock_chat_bindings, scheduler::main, 250ms);
	}
}
