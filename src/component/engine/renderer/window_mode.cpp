#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/engine/console/console.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>

namespace renderer_window_mode
{
	namespace
	{
		std::uintptr_t native_window_parms{};
		bool installed{};
		struct call_site
		{
			std::uintptr_t address;
			std::array<unsigned char, 5> original;
			std::array<unsigned char, 5> patch{};
		};
		std::array<call_site, 3> calls{{
			{0x103BEB48, {0xE8, 0x13, 0xF5, 0xFF, 0xFF}},
			{0x103BEF74, {0xE8, 0xE7, 0xF0, 0xFF, 0xFF}},
			{0x103BF006, {0xE8, 0x55, 0xF0, 0xFF, 0xFF}},
		}};

		void __cdecl apply_borderless_parms(unsigned char* parms)
		{
			const auto* borderless = game::Dvar_FindVar("r_borderless");
			if (!parms || parms[8] || !borderless) return;
			const bool enabled = borderless->type == game::DVAR_TYPE_STRING
				? borderless->current.string && std::atoi(borderless->current.string) != 0
				: borderless->current.enabled;
			if (!enabled) return;

			// QoS PC R_GetWindowParms (103BE060), compared with KisakCOD's
			// window-parms/StoreWindowSettings flow. Only verified prefix fields
			// are accessed; the closed-source renderer retains device ownership.
			auto& x = *reinterpret_cast<int*>(parms + 12);
			auto& y = *reinterpret_cast<int*>(parms + 16);
			MONITORINFO monitor{sizeof(MONITORINFO)};
			if (!GetMonitorInfoA(MonitorFromPoint({x, y}, MONITOR_DEFAULTTONEAREST), &monitor)) return;
			const int width = monitor.rcMonitor.right - monitor.rcMonitor.left;
			const int height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
			if (width <= 0 || height <= 0) return;
			x = monitor.rcMonitor.left;
			y = monitor.rcMonitor.top;
			*reinterpret_cast<int*>(parms + 24) = width; // scene width
			*reinterpret_cast<int*>(parms + 28) = height;
			*reinterpret_cast<int*>(parms + 32) = width; // display width
			*reinterpret_cast<int*>(parms + 36) = height;
			parms[20] = static_cast<float>(width) / height > 1.5f;
			const auto* wide = *reinterpret_cast<game::dvar_s**>(game::game_offset(0x10752C58));
			if (wide && wide->name && wide->type == game::DVAR_TYPE_BOOL)
				game::Dvar_SetFromStringByName(wide->name, parms[20] ? "1" : "0");
		}

		__declspec(naked) void window_parms_stub()
		{
			// All three verified callers supply the output buffer in EDI. Invoke
			// native code first, preserving its EAX result, flags and register state.
			__asm
			{
				call dword ptr [native_window_parms]
				pushfd
				pushad
				mov esi, esp
				sub esp, 528
				and esp, -16
				fxsave [esp]
				push edi
				call apply_borderless_parms
				add esp, 4
				fxrstor [esp]
				mov esp, esi
				popad
				popfd
				ret
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// Atomic compatibility gate; leave stock calls untouched on mismatch.
			for (const auto& call : calls)
			{
				const auto site = game::game_offset(call.address);
				if (std::memcmp(reinterpret_cast<const void*>(site), call.original.data(), 5) != 0
					|| utils::hook::is_relatively_far(reinterpret_cast<void*>(site), reinterpret_cast<void*>(window_parms_stub)))
				{
					console::warn("[renderer] fullscreen windowed skipped: native window-parms calls unavailable\n");
					return;
				}
			}
			native_window_parms = game::game_offset(0x103BE060);
			for (auto& call : calls)
			{
				const auto site = game::game_offset(call.address);
				utils::hook::call(site, window_parms_stub);
				std::memcpy(call.patch.data(), reinterpret_cast<const void*>(site), 5);
			}
			installed = true;
		}

		void pre_destroy() override
		{
			if (!installed) return;
			for (const auto& call : calls)
			{
				const auto site = game::game_offset(call.address);
				if (std::memcmp(reinterpret_cast<const void*>(site), call.patch.data(), 5) == 0)
					utils::hook::set(site, call.original);
			}
			installed = false;
		}
	};
}

REGISTER_COMPONENT(renderer_window_mode::component)
