#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>

// Patch mouse input to use Windows raw input while preserving the game's
// existing mouse event flow and recenter behavior.

namespace mouse_input
{
	namespace
	{
		utils::hook::detour in_mouse_move_hook;

		WNDPROC original_wnd_proc = nullptr;
		bool wnd_proc_hooked = false;
		bool raw_input_registered = false;
		LONG raw_mouse_x = 0;
		LONG raw_mouse_y = 0;

		HWND get_window()
		{
			return *game::main_window;
		}

		int cl_mouse_event(const int x, const int y, const int dx, const int dy)
		{
			const auto func = game::game_offset(0x102FD5D0);
			int result = 0;

			__asm
			{
				push dy
				push dx
				mov edx, y
				mov ecx, x
				call func
				add esp, 8
				mov result, eax
			}

			return result;
		}

		void recenter_mouse()
		{
			utils::hook::invoke<void>(game::game_offset(0x102C35B0));
		}

		void clamp_mouse_move(POINT& point)
		{
			RECT rect{};
			GetWindowRect(get_window(), &rect);

			bool changed = false;

			if (point.x < rect.left)
			{
				point.x = rect.left;
				changed = true;
			}
			else if (point.x >= rect.right)
			{
				point.x = rect.right - 1;
				changed = true;
			}

			if (point.y < rect.top)
			{
				point.y = rect.top;
				changed = true;
			}
			else if (point.y >= rect.bottom)
			{
				point.y = rect.bottom - 1;
				changed = true;
			}

			if (changed)
			{
				SetCursorPos(point.x, point.y);
			}
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
			}
		}

		LRESULT CALLBACK wnd_proc_stub(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			if (msg == WM_INPUT)
			{
				RAWINPUT raw{};
				UINT size = sizeof(raw);
				if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == size
					&& raw.header.dwType == RIM_TYPEMOUSE)
				{
					if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
					{
						raw_mouse_x = raw.data.mouse.lLastX;
						raw_mouse_y = raw.data.mouse.lLastY;
					}
					else
					{
						raw_mouse_x += raw.data.mouse.lLastX;
						raw_mouse_y += raw.data.mouse.lLastY;
					}
				}
			}

			return CallWindowProcA(original_wnd_proc, hwnd, msg, wparam, lparam);
		}

		void install_wnd_proc()
		{
			if (wnd_proc_hooked || !get_window())
			{
				return;
			}

			original_wnd_proc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrA(get_window(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wnd_proc_stub))
			);

			if (original_wnd_proc)
			{
				wnd_proc_hooked = true;
				register_raw_input();
			}
		}

		void uninstall_wnd_proc()
		{
			if (!wnd_proc_hooked || !get_window() || !original_wnd_proc)
			{
				return;
			}

			SetWindowLongPtrA(get_window(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wnd_proc));
			original_wnd_proc = nullptr;
			wnd_proc_hooked = false;
			raw_input_registered = false;
		}

		void in_mouse_move_stub()
		{
			if (!dvars::m_rawInput || !dvars::m_rawInput->current.enabled || !wnd_proc_hooked)
			{
				in_mouse_move_hook.invoke<void>();
				return;
			}

			const auto hwnd = get_window();
			if (!hwnd || GetForegroundWindow() != hwnd)
			{
				return;
			}

			static LONG old_x = 0;
			static LONG old_y = 0;

			POINT cursor{};
			GetCursorPos(&cursor);

			if (const auto* fullscreen = game::Dvar_FindVar("r_fullscreen");
				fullscreen && fullscreen->current.enabled)
			{
				clamp_mouse_move(cursor);
			}

			const auto dx = raw_mouse_x - old_x;
			const auto dy = raw_mouse_y - old_y;
			old_x = raw_mouse_x;
			old_y = raw_mouse_y;

			ScreenToClient(hwnd, &cursor);

			if (cl_mouse_event(cursor.x, cursor.y, dx, dy) != 0 && (dx != 0 || dy != 0))
			{
				recenter_mouse();
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			in_mouse_move_hook.create(game::game_offset(0x102C3970), &in_mouse_move_stub);

			scheduler::loop([]
				{
					install_wnd_proc();
				}, scheduler::main, 100ms);

			scheduler::on_shutdown([]
				{
					uninstall_wnd_proc();
					in_mouse_move_hook.clear();
				});
		}
	};
}

REGISTER_COMPONENT(mouse_input::component)
