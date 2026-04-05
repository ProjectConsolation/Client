#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "gamepad.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>

namespace mouse_input
{
	namespace
	{
		WNDPROC original_wnd_proc = nullptr;
		bool callsite_patched = false;
		bool wnd_proc_hooked = false;
		bool raw_input_registered = false;
		int ready_ticks = 0;
		LONG raw_mouse_x = 0;
		LONG raw_mouse_y = 0;
		std::array<std::uint8_t, 5> original_mouse_event_call{};

		struct mouse_event_state
		{
			int x = 0;
			int y = 0;
			int dx = 0;
			int dy = 0;
		};

		mouse_event_state pending_event{};
		mouse_event_state original_call_state{};
		int original_call_result = 0;
		const std::uint32_t original_mouse_event_address = game::game_offset(0x102FD5D0);

		int __cdecl mouse_event_body(int y, int x, int dx, int dy);
		void mouse_event_callsite_stub();
		void call_original_mouse_event_stub();

		HWND get_window()
		{
			return *game::main_window;
		}

		bool is_window_ready()
		{
			const auto hwnd = get_window();
			return hwnd && IsWindow(hwnd) && dvars::m_rawInput != nullptr;
		}

		void register_raw_input()
		{
			if (raw_input_registered || !get_window())
			{
				return;
			}

			RAWINPUTDEVICE device{};
			device.usUsagePage = 0x01;
			device.usUsage = 0x02;
			device.dwFlags = RIDEV_INPUTSINK;
			device.hwndTarget = get_window();

			if (RegisterRawInputDevices(&device, 1, sizeof(device)))
			{
				raw_input_registered = true;
				raw_mouse_x = 0;
				raw_mouse_y = 0;
			}
		}

		void unregister_raw_input()
		{
			if (!raw_input_registered)
			{
				return;
			}

			RAWINPUTDEVICE device{};
			device.usUsagePage = 0x01;
			device.usUsage = 0x02;
			device.dwFlags = RIDEV_REMOVE;
			device.hwndTarget = nullptr;
			RegisterRawInputDevices(&device, 1, sizeof(device));

			raw_input_registered = false;
			raw_mouse_x = 0;
			raw_mouse_y = 0;
		}

		LRESULT CALLBACK wnd_proc_stub(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			if (msg == WM_ACTIVATEAPP && wparam == FALSE)
			{
				raw_mouse_x = 0;
				raw_mouse_y = 0;
			}

			if (msg == WM_INPUT && raw_input_registered)
			{
				UINT size = sizeof(RAWINPUT);
				static BYTE raw_bytes[sizeof(RAWINPUT)]{};
				if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, raw_bytes, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1))
				{
					const auto* raw = reinterpret_cast<const RAWINPUT*>(raw_bytes);
					if (raw->header.dwType == RIM_TYPEMOUSE)
					{
						raw_mouse_x += raw->data.mouse.lLastX;
						raw_mouse_y += raw->data.mouse.lLastY;
					}
				}
			}

			return CallWindowProcA(original_wnd_proc, hwnd, msg, wparam, lparam);
		}

		void install_hooks_if_ready()
		{
			if (callsite_patched)
			{
				return;
			}

			if (!is_window_ready())
			{
				ready_ticks = 0;
				return;
			}

			if (++ready_ticks < 10)
			{
				return;
			}

			for (auto i = 0u; i < original_mouse_event_call.size(); ++i)
			{
				original_mouse_event_call[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102C3934 + i));
			}

			utils::hook::call(game::game_offset(0x102C3934), mouse_event_callsite_stub);
			callsite_patched = true;
		}

		void restore_mouse_event_callsite()
		{
			if (!callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_mouse_event_call.size(); ++i)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x102C3934 + static_cast<int>(i)), original_mouse_event_call[i]);
			}

			callsite_patched = false;
		}

		void install_wnd_proc_if_ready()
		{
			if (wnd_proc_hooked || !callsite_patched || !is_window_ready())
			{
				return;
			}

			original_wnd_proc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrA(get_window(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wnd_proc_stub))
			);

			if (original_wnd_proc)
			{
				wnd_proc_hooked = true;
			}
		}

		void update_raw_input_state()
		{
			if (!wnd_proc_hooked || !dvars::m_rawInput)
			{
				return;
			}

			if (dvars::m_rawInput->current.enabled)
			{
				register_raw_input();
			}
			else
			{
				unregister_raw_input();
			}
		}

		void uninstall_wnd_proc()
		{
			if (!wnd_proc_hooked || !get_window() || !original_wnd_proc)
			{
				return;
			}

			unregister_raw_input();
			SetWindowLongPtrA(get_window(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wnd_proc));
			original_wnd_proc = nullptr;
			wnd_proc_hooked = false;
		}

		int call_original_mouse_event(const mouse_event_state& state)
		{
			original_call_state = state;
			call_original_mouse_event_stub();
			return original_call_result;
		}

		int __cdecl mouse_event_body(int y, int x, int dx, int dy)
		{
			pending_event.x = x;
			pending_event.y = y;
			pending_event.dx = dx;
			pending_event.dy = dy;

			if (!dvars::m_rawInput || !dvars::m_rawInput->current.enabled || !wnd_proc_hooked || !raw_input_registered)
			{
				return call_original_mouse_event(pending_event);
			}

			if ((*game::keyCatchers & 0x10) != 0)
			{
				return call_original_mouse_event(pending_event);
			}

			const auto hwnd = get_window();
			if (!hwnd || GetForegroundWindow() != hwnd)
			{
				return call_original_mouse_event(pending_event);
			}

			static LONG old_x = 0;
			static LONG old_y = 0;

			const auto raw_dx = raw_mouse_x - old_x;
			const auto raw_dy = raw_mouse_y - old_y;
			old_x = raw_mouse_x;
			old_y = raw_mouse_y;

			if (raw_dx == 0 && raw_dy == 0)
			{
				if (gamepad::should_override_mouse())
				{
					int gamepad_dx = 0;
					int gamepad_dy = 0;
					if (gamepad::consume_right_stick_delta(gamepad_dx, gamepad_dy))
					{
						pending_event.dx = gamepad_dx;
						pending_event.dy = gamepad_dy;
						return call_original_mouse_event(pending_event);
					}
				}

				return call_original_mouse_event(pending_event);
			}

			pending_event.dx = static_cast<int>(raw_dx);
			pending_event.dy = static_cast<int>(raw_dy);
			return call_original_mouse_event(pending_event);
		}

		__declspec(naked) void mouse_event_callsite_stub()
		{
			__asm
			{
				push esi
				push edi
				push ebx

				mov esi, ecx
				mov edi, edx
				mov ebx, [esp+10h]
				mov eax, [esp+14h]

				push eax
				push ebx
				push edi
				push esi
				call mouse_event_body
				add esp, 10h

				pop ebx
				pop edi
				pop esi
				ret
			}
		}

		__declspec(naked) void call_original_mouse_event_stub()
		{
			__asm
			{
				push esi
				mov esi, offset original_call_state

				push [esi + 12]
				push [esi + 8]
				mov ecx, [esi + 4]
				mov edx, [esi + 0]
				mov eax, original_mouse_event_address
				call eax
				add esp, 8
				mov dword ptr [original_call_result], eax

				pop esi
				ret
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::loop([]
				{
					install_hooks_if_ready();
					install_wnd_proc_if_ready();
					update_raw_input_state();
				}, scheduler::main, 100ms);

			scheduler::on_shutdown([]
				{
					uninstall_wnd_proc();

					restore_mouse_event_callsite();
				});
		}

		void pre_destroy() override
		{
			uninstall_wnd_proc();

			restore_mouse_event_callsite();
		}
	};
}

REGISTER_COMPONENT(mouse_input::component)
