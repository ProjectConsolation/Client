#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/gamepad/gamepad.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include "menu_capture_policy.hpp"

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
		LONG old_raw_x = 0;
		LONG old_raw_y = 0;
		bool was_foreground = false;
		bool native_mouse_state_verified = false;
		bool menu_clip_hooked = false;
		bool menu_cursor_free = false;
		std::array<std::uint8_t, 6> original_clip_call{};
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

		void reset_raw_motion()
		{
			raw_mouse_x = raw_mouse_y = old_raw_x = old_raw_y = 0;
		}

		void verify_native_mouse_state()
		{
			// QoS PC 1.1 IN_Frame clears s_wmv.mouseActive here. Validate the
			// relocated absolute operand before writing this byte ourselves.
			std::array<std::uint8_t, 7> expected{0xC6, 0x05, 0, 0, 0, 0, 0};
			const auto address = static_cast<std::uint32_t>(game::game_offset(0x113F00E8));
			std::memcpy(expected.data() + 2, &address, sizeof(address));
			native_mouse_state_verified = std::memcmp(reinterpret_cast<void*>(game::game_offset(0x102C39D2)),
				expected.data(), expected.size()) == 0;
			if (!native_mouse_state_verified)
				game::Com_Printf(0, "[mouse] focus-release native state guard failed; Windows release only\n");
		}

		bool native_mouse_active()
		{
			return native_mouse_state_verified && *reinterpret_cast<const bool*>(game::game_offset(0x113F00E8));
		}

		bool wants_free_menu_cursor()
		{
			const auto hwnd = get_window();
			if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
			const auto* fullscreen = game::Dvar_FindVar("r_fullscreen");
			const auto* bypass = game::Dvar_FindVar("cl_bypassMouseInput");
			return fullscreen && bypass && free_menu_cursor(GetForegroundWindow() == hwnd,
				!fullscreen->current.enabled, *game::keyCatchers,
				*reinterpret_cast<const int*>(game::game_offset(0x113D12E8)), bypass->current.enabled);
		}

		void unclip_game_window(HWND hwnd)
		{
			RECT clip{}, window{};
			if (GetClipCursor(&clip) && GetWindowRect(hwnd, &window) && EqualRect(&clip, &window))
				ClipCursor(nullptr);
			if (GetCapture() == hwnd) ReleaseCapture();
		}

		BOOL __stdcall clip_game_mouse(const RECT* rect)
		{
			// Dedicated QoS IN_ActivateWin32Mouse ClipCursor call. No process-wide
			// API detour: menus remain absolute, gameplay/fullscreen retain native clip.
			return ClipCursor(wants_free_menu_cursor() ? nullptr : rect);
		}

		void install_menu_clip_hook()
		{
			const auto site = game::game_offset(0x102C364B);
			std::array<std::uint8_t, 6> expected{0xFF, 0x15, 0, 0, 0, 0};
			const auto slot = static_cast<std::uint32_t>(game::game_offset(0x1047632C));
			std::memcpy(expected.data() + 2, &slot, sizeof(slot));
			if (!native_mouse_state_verified || std::memcmp(reinterpret_cast<void*>(site), expected.data(), expected.size()))
			{
				game::Com_Printf(0, "[mouse] windowed menu cursor hook skipped: unexpected engine instructions\n");
				return;
			}
			original_clip_call = expected;
			utils::hook::call(site, &clip_game_mouse);
			utils::hook::set<std::uint8_t>(site + 5, 0x90);
			menu_clip_hooked = true;
		}

		void update_menu_capture()
		{
			if (!menu_clip_hooked) return;
			if (!get_window()) { menu_cursor_free = false; return; }
			const bool free = wants_free_menu_cursor();
			const bool was_free = menu_cursor_free;
			menu_cursor_free = free; // Publish before native calls can dispatch messages.
			if (free)
			{
				unclip_game_window(get_window());
				if (!was_free) reset_raw_motion();
			}
			else if (was_free && GetForegroundWindow() == get_window() && !IsIconic(get_window()))
			{
				// QoS PC 102C36D0 is cdecl(force). Native mouse enablement and
				// foreground checks remain authoritative; do not force capture ourselves.
				reset_raw_motion();
				utils::hook::invoke<void>(game::game_offset(0x102C36D0), 1);
			}
		}

		void release_background_mouse(HWND hwnd)
		{
			reset_raw_motion();
			// QoS IN_Activate(102C3720)/IN_Frame(102C3970) can skip unclip
			// when mouseActive was already cleared. Leave app-active state alone;
			// clearing only mouseActive lets native IN_Frame reacquire on return.
			if (native_mouse_state_verified)
				*reinterpret_cast<bool*>(game::game_offset(0x113F00E8)) = false;
			// ClipCursor is desktop-global: do not undo another application's
			// newly installed clip during the delayed foreground fallback.
			unclip_game_window(hwnd);
			// Normalize visibility without incrementing its counter on duplicate
			// deactivation messages. Native activation restores its own visibility.
			auto count = ShowCursor(TRUE);
			if (count > 0) ShowCursor(FALSE);
			for (unsigned attempt = 0; count < 0 && attempt < 32; ++attempt)
				count = ShowCursor(TRUE);
			was_foreground = false;
		}

		void update_focus_state()
		{
			const auto hwnd = get_window();
			if (!wnd_proc_hooked || !hwnd) return;
			const bool foreground = GetForegroundWindow() == hwnd && !IsIconic(hwnd);
			if (!foreground && (was_foreground || native_mouse_active()))
				release_background_mouse(hwnd);
			else if (foreground && !was_foreground)
				reset_raw_motion();
			was_foreground = foreground;
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
				reset_raw_motion();
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
			reset_raw_motion();
		}

		LRESULT CALLBACK wnd_proc_stub(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			const bool foreground = GetForegroundWindow() == hwnd;
			if (foreground && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN
				|| msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN
				|| msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL))
				gamepad::note_mouse_activity();
			const bool losing_focus = (msg == WM_ACTIVATEAPP && wparam == FALSE)
				|| (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)
				|| msg == WM_KILLFOCUS || (msg == WM_SIZE && wparam == SIZE_MINIMIZED);

			// Gameplay recenters the legacy cursor synthetically. Its native delta
			// path below reports real movement; absolute messages are for UI only.
			if (foreground && msg == WM_MOUSEMOVE && !raw_input_registered
				&& (*game::keyCatchers & 0x10) != 0)
			{
				gamepad::note_mouse_activity();
			}

			if (foreground && msg == WM_SETCURSOR && gamepad::should_hide_cursor())
			{
				SetCursor(nullptr);
				return TRUE;
			}

			if (msg == WM_INPUT && raw_input_registered)
			{
				UINT size = sizeof(RAWINPUT);
				static BYTE raw_bytes[sizeof(RAWINPUT)]{};
				if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, raw_bytes, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1))
				{
					const auto* raw = reinterpret_cast<const RAWINPUT*>(raw_bytes);
					if (foreground && raw->header.dwType == RIM_TYPEMOUSE)
					{
						raw_mouse_x += raw->data.mouse.lLastX;
						raw_mouse_y += raw->data.mouse.lLastY;
						if (foreground && (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0
							|| raw->data.mouse.usButtonFlags != 0))
						{
							gamepad::note_mouse_activity();
						}
					}
				}
			}

			// Preserve native key clearing, activation and renderer handling first.
			// Then release even when its mouseActive guard skipped deactivation.
			const auto result = CallWindowProcA(original_wnd_proc, hwnd, msg, wparam, lparam);
			if (losing_focus) release_background_mouse(hwnd);
			else update_menu_capture();
			return result;
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
				verify_native_mouse_state();
				install_menu_clip_hook();
				was_foreground = GetForegroundWindow() == get_window();
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
			unregister_raw_input();
			if (menu_clip_hooked)
				utils::hook::set(game::game_offset(0x102C364B), original_clip_call);
			menu_clip_hooked = false;
			menu_cursor_free = false;
			// Restore code even if WM_DESTROY already cleared the game's HWND.
			if (wnd_proc_hooked && get_window() && original_wnd_proc)
				SetWindowLongPtrA(get_window(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wnd_proc));
			original_wnd_proc = nullptr;
			wnd_proc_hooked = false;
		}

		int call_original_mouse_event(const mouse_event_state& state)
		{
			update_menu_capture();
			original_call_state = state;
			call_original_mouse_event_stub();
			return original_call_result;
		}

		int __cdecl mouse_event_body(int y, int x, int dx, int dy)
		{
			if (GetForegroundWindow() != get_window())
			{
				reset_raw_motion();
				return 0;
			}
			pending_event.x = x;
			pending_event.y = y;
			pending_event.dx = dx;
			pending_event.dy = dy;

			if (!raw_input_registered && (dx != 0 || dy != 0) && (*game::keyCatchers & 0x10) == 0
				&& GetForegroundWindow() == get_window())
			{
				gamepad::note_mouse_activity();
			}

			if (gamepad::should_hide_cursor() && (*game::keyCatchers & 0x10) == 0)
			{
				return 0;
			}

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

			const auto raw_dx = raw_mouse_x - old_raw_x;
			const auto raw_dy = raw_mouse_y - old_raw_y;
			old_raw_x = raw_mouse_x;
			old_raw_y = raw_mouse_y;

			if (raw_dx == 0 && raw_dy == 0)
			{
				return call_original_mouse_event(pending_event);
			}

			gamepad::note_mouse_activity();
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
					update_focus_state();
					update_menu_capture();
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
