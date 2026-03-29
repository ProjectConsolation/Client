#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "scheduler.hpp"

#include "game/game.hpp"
#include "game_console.hpp"

#include <utils/hook.hpp>

namespace game_console
{
	namespace
	{
		utils::hook::detour cl_key_event_hook;
		utils::hook::detour cl_console_print_hook;
		utils::hook::detour con_draw_solid_console_hook;

		float overlay_back_color[4] = { 0.08f, 0.09f, 0.10f, 0.88f };
		float overlay_border_color[4] = { 0.18f, 0.55f, 0.35f, 1.0f };
		float overlay_text_color[4] = { 0.95f, 0.96f, 0.97f, 1.0f };
		float overlay_hint_color[4] = { 0.65f, 0.75f, 0.70f, 1.0f };

		constexpr auto max_console_lines = 128u;
		constexpr auto max_history_entries = 32u;
		constexpr auto max_input_chars = 240u;
		constexpr auto restrict_console_dvar_ptr = 0x112666BC;
		constexpr auto devgui_dvar_ptr = 0x10711ABC;
		constexpr auto splitscreen_dvar_ptr = 0x1130BF48;
		constexpr auto console_local_ready = 0x11263D9C;

		bool overlay_active = false;
		bool process_shutting_down = false;
		bool render_debug_pending = false;
		bool was_f1_down = false;
		bool was_oem5_down = false;
		bool was_oem102_down = false;
		std::array<bool, 256> key_was_down{};

		struct console_state
		{
			std::string input{};
			std::size_t cursor = 0;
			int history_index = -1;
			std::vector<std::string> history{};
			std::vector<std::string> lines{};
		};

		console_state* con = nullptr;

		game::Font_s* get_console_font()
		{
			if (game::con_font && *game::con_font)
			{
				return *game::con_font;
			}

			return game::R_RegisterFont("fonts/consoleFont");
		}

		game::Material* get_white_material()
		{
			return game::Material_RegisterHandle("white");
		}

		HWND get_window()
		{
			return *game::main_window;
		}

		bool is_game_focused()
		{
			const auto hwnd = get_window();
			return hwnd && GetForegroundWindow() == hwnd;
		}

		bool is_key_down(const int vk)
		{
			return (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		void trim_console_lines()
		{
			if (!con)
			{
				return;
			}

			if (con->lines.size() > max_console_lines)
			{
				const auto overflow = con->lines.size() - max_console_lines;
				con->lines.erase(con->lines.begin(), con->lines.begin() + static_cast<std::ptrdiff_t>(overflow));
			}
		}

		void trim_history()
		{
			if (!con)
			{
				return;
			}

			if (con->history.size() > max_history_entries)
			{
				const auto overflow = con->history.size() - max_history_entries;
				con->history.erase(con->history.begin(), con->history.begin() + static_cast<std::ptrdiff_t>(overflow));
			}
		}

		std::string strip_colors(std::string_view text)
		{
			std::string cleaned{};
			cleaned.reserve(text.size());

			for (std::size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] == '^' && (i + 1) < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))
				{
					++i;
					continue;
				}

				if (text[i] != '\r')
				{
					cleaned.push_back(text[i]);
				}
			}

			return cleaned;
		}

		void append_line(const std::string& line)
		{
			if (!con)
			{
				return;
			}

			con->lines.push_back(line);
			trim_console_lines();
		}

		void append_console_text(const char* text)
		{
			if (!text || !*text)
			{
				return;
			}

			const auto cleaned = strip_colors(text);
			std::size_t start = 0;

			while (start <= cleaned.size())
			{
				const auto end = cleaned.find('\n', start);
				if (end == std::string::npos)
				{
					const auto tail = cleaned.substr(start);
					if (!tail.empty())
					{
						append_line(tail);
					}
					break;
				}

				append_line(cleaned.substr(start, end - start));
				start = end + 1;
			}
		}

		void cl_console_print_stub(int local_client_num, int channel, const char* txt, int duration, int pixel_width, int flags)
		{
			append_console_text(txt);
			cl_console_print_hook.invoke<void>(local_client_num, channel, txt, duration, pixel_width, flags);
		}

		bool dvar_enabled(const std::uintptr_t ptr_address)
		{
			const auto dvar = *reinterpret_cast<std::uintptr_t*>(game::game_offset(ptr_address));
			return dvar && *reinterpret_cast<unsigned char*>(dvar + 16) != 0;
		}

		bool can_toggle_console()
		{
			const auto local_ready = *reinterpret_cast<int*>(game::game_offset(console_local_ready)) != 0;
			const auto console_unrestricted = !dvar_enabled(restrict_console_dvar_ptr);
			const auto devgui_enabled = dvar_enabled(devgui_dvar_ptr);
			const auto splitscreen_disabled = !dvar_enabled(splitscreen_dvar_ptr);

			return local_ready
				&& ((*game::keyCatchers & 1) != 0 || devgui_enabled || splitscreen_disabled)
				&& (console_unrestricted || (*game::keyCatchers & 1) != 0);
		}

		void debug_log_toggle(const char* key_name)
		{
#ifdef DEBUG
			const auto local_ready = *reinterpret_cast<int*>(game::game_offset(console_local_ready)) != 0;
			const auto console_unrestricted = !dvar_enabled(restrict_console_dvar_ptr);
			const auto devgui_enabled = dvar_enabled(devgui_dvar_ptr);
			const auto splitscreen_disabled = !dvar_enabled(splitscreen_dvar_ptr);

			game::Com_Printf(0,
				"^1debug:^3 game_console.cpp: key=%s pressed, state=%s, visible=%d, can_toggle=%d, local_ready=%d, unrestricted=%d, devgui=%d, splitscreen_disabled=%d\n",
				key_name, is_active() ? "open" : "closed", is_active() ? 1 : 0, can_toggle_console() ? 1 : 0,
				local_ready ? 1 : 0, console_unrestricted ? 1 : 0, devgui_enabled ? 1 : 0, splitscreen_disabled ? 1 : 0);
#else
			(void)key_name;
#endif
		}

		void set_overlay_active(const bool active)
		{
			overlay_active = active;
			key_was_down.fill(false);

			if (overlay_active && con)
			{
				con->history_index = -1;
				render_debug_pending = true;
			}
		}

		void toggle_overlay_state()
		{
			set_overlay_active(!overlay_active);
		}

		void handle_toggle_press(const char* key_name)
		{
			toggle_overlay_state();
			debug_log_toggle(key_name);
		}

		struct overlay_view
		{
			float left;
			float top;
			float width;
			float height;
		};

		overlay_view get_overlay_view()
		{
			overlay_view view{ 0.0f, 0.0f, 640.0f, 480.0f };
			const auto scr_place = game::ScrPlace_GetViewPlacement();

			view.left = scr_place.realViewableMin[0];
			view.top = scr_place.realViewableMin[1];
			view.width = scr_place.realViewableMax[0] - scr_place.realViewableMin[0];
			view.height = scr_place.realViewableMax[1] - scr_place.realViewableMin[1];

			if (view.width <= 0.0f)
			{
				view.width = scr_place.realViewportSize[0];
			}

			if (view.height <= 0.0f)
			{
				view.height = scr_place.realViewportSize[1];
			}

			if (view.width <= 0.0f || view.height <= 0.0f)
			{
				const auto hwnd = get_window();
				RECT client_rect{};
				if (hwnd && GetClientRect(hwnd, &client_rect))
				{
					view.left = 0.0f;
					view.top = 0.0f;
					view.width = static_cast<float>(client_rect.right - client_rect.left);
					view.height = static_cast<float>(client_rect.bottom - client_rect.top);
				}
			}

			return view;
		}

		void draw_box(float x, float y, float width, float height, float* fill_color, float* border_color)
		{
			auto* const material = get_white_material();
			if (!material)
			{
				return;
			}

			const auto border_w = std::max(1.0f, width * 0.003125f);
			const auto border_h = std::max(1.0f, height * 0.00625f);

			game::R_AddCmdDrawStretchPic(x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, fill_color, material, 0);
			game::R_AddCmdDrawStretchPic(x, y, width, border_h, 0.0f, 0.0f, 1.0f, 1.0f, border_color, material, 0);
			game::R_AddCmdDrawStretchPic(x, y + height - border_h, width, border_h, 0.0f, 0.0f, 1.0f, 1.0f, border_color, material, 0);
			game::R_AddCmdDrawStretchPic(x, y, border_w, height, 0.0f, 0.0f, 1.0f, 1.0f, border_color, material, 0);
			game::R_AddCmdDrawStretchPic(x + width - border_w, y, border_w, height, 0.0f, 0.0f, 1.0f, 1.0f, border_color, material, 0);
		}

		void draw_text(const char* text, float x, float y, float* color, float scale = 1.0f)
		{
			auto* const font = get_console_font();
			if (!font || !text)
			{
				return;
			}

			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x, y, scale, scale, 0.0f, color, 0);
		}

		float get_line_height(const float scale)
		{
			auto* const font = get_console_font();
			if (!font)
			{
				return 18.0f;
			}

			return static_cast<float>(font->pixelHeight) * scale;
		}

		void draw_input_line(const float x, const float y, const float scale)
		{
			if (!con)
			{
				return;
			}

			const std::string prompt = "] " + con->input;
			draw_text(prompt.c_str(), x, y, overlay_text_color, scale);

			const auto blink_on = ((GetTickCount() / 500u) & 1u) == 0u;
			if (!blink_on)
			{
				return;
			}

			auto* const font = get_console_font();
			if (!font)
			{
				return;
			}

			const auto prefix = prompt.substr(0, 2 + con->cursor);
			const auto cursor_x = x + (static_cast<float>(game::R_TextWidth(prefix.c_str(), 0x7FFFFFFF, font)) * scale);
			draw_text("_", cursor_x, y, overlay_text_color, scale);
		}

		void draw_console_overlay()
		{
			if (!is_active() || process_shutting_down)
			{
				return;
			}

			if (!con)
			{
				return;
			}

			const auto view = get_overlay_view();
			if (view.width <= 0.0f || view.height <= 0.0f)
			{
				return;
			}

#ifdef DEBUG
			if (render_debug_pending)
			{
				const auto hwnd = get_window();
				RECT client_rect{};
				if (hwnd)
				{
					GetClientRect(hwnd, &client_rect);
				}
				auto* const font = get_console_font();
				auto* const material = get_white_material();
				game::Com_Printf(0,
					"^1debug:^3 game_console.cpp: render active hwnd=%p size=%dx%d font=%p material=%p lines=%zu\n",
					hwnd,
					client_rect.right - client_rect.left,
					client_rect.bottom - client_rect.top,
					font,
					material,
					con->lines.size());
				render_debug_pending = false;
			}
#endif

			const float x = view.left + 8.0f;
			const float y = view.top + 8.0f;
			const float width = std::max(320.0f, view.width - 16.0f);
			const float height = std::max(180.0f, view.height * 0.58f);
			const float scale = std::max(0.72f, view.height / 1080.0f);
			const float line_height = get_line_height(scale) + 2.0f;

			draw_box(x, y, width, height, overlay_back_color, overlay_border_color);
			draw_box(x + 16.0f, y + height - 58.0f, width - 32.0f, 34.0f, overlay_back_color, overlay_border_color);

			draw_text("Project Consolation Console", x + 18.0f, y + 30.0f, overlay_text_color, 1.0f);
			draw_text("F1, | and \\\\ toggle. Enter executes. Up/Down browse history.", x + 18.0f, y + 54.0f, overlay_hint_color, 0.8f);

			const float log_top = y + 84.0f;
			const float log_bottom = y + height - 72.0f;
			const auto visible_lines = std::max(0, static_cast<int>((log_bottom - log_top) / line_height));
			const auto first_line = std::max(0, static_cast<int>(con->lines.size()) - visible_lines);

			float line_y = log_top;
			for (int i = first_line; i < static_cast<int>(con->lines.size()); ++i)
			{
				draw_text(con->lines[static_cast<std::size_t>(i)].c_str(), x + 18.0f, line_y, overlay_text_color, scale);
				line_y += line_height;
			}

			draw_input_line(x + 24.0f, y + height - 34.0f, 0.9f);
		}

		bool should_swallow_key_event()
		{
			return overlay_active && is_game_focused();
		}

		void call_original_cl_key_event(const int local_client_num, const int key, const int down, const unsigned int time)
		{
			int func_loc = static_cast<int>(reinterpret_cast<std::uintptr_t>(cl_key_event_hook.get_original()));

			__asm
			{
				mov ecx, local_client_num;
				push time;
				push down;
				push key;
				call func_loc;
				add esp, 0Ch;
			}
		}

		char __cdecl cl_key_event_body(const int local_client_num, const int key, const int down, const unsigned int time)
		{
			(void)local_client_num;
			(void)key;
			(void)down;
			(void)time;

			return should_swallow_key_event() ? 1 : 0;
		}

		__declspec(naked) void cl_key_event_stub()
		{
			__asm
			{
				push esi;
				push edi;
				push ebx;

				mov esi, ecx;
				mov edi, [esp+10h];
				mov ebx, [esp+14h];
				mov eax, [esp+18h];

				push eax;
				push ebx;
				push edi;
				push esi;
				call cl_key_event_body;
				add esp, 10h;

				test al, al;
				jnz swallow_key;

				mov eax, [esp+18h];
				push eax;
				push ebx;
				push edi;
				push esi;
				call call_original_cl_key_event;
				add esp, 10h;

			swallow_key:
				pop ebx;
				pop edi;
				pop esi;
				ret;
			}
		}

		void con_draw_solid_console_stub()
		{
			draw_console_overlay();
			con_draw_solid_console_hook.invoke<void>();
		}

		void execute_input()
		{
			if (!con || con->input.empty())
			{
				return;
			}

			append_line("] " + con->input);

			if (con->history.empty() || con->history.back() != con->input)
			{
				con->history.push_back(con->input);
				trim_history();
			}

			const auto command = con->input + "\n";
			game::Cbuf_AddText(0, command.c_str());

			con->input.clear();
			con->cursor = 0;
			con->history_index = -1;
		}

		void history_up()
		{
			if (!con || con->history.empty())
			{
				return;
			}

			if (con->history_index < 0)
			{
				con->history_index = static_cast<int>(con->history.size()) - 1;
			}
			else if (con->history_index > 0)
			{
				--con->history_index;
			}

			con->input = con->history[static_cast<std::size_t>(con->history_index)];
			con->cursor = con->input.size();
		}

		void history_down()
		{
			if (!con || con->history.empty() || con->history_index < 0)
			{
				return;
			}

			++con->history_index;
			if (con->history_index >= static_cast<int>(con->history.size()))
			{
				con->history_index = -1;
				con->input.clear();
				con->cursor = 0;
				return;
			}

			con->input = con->history[static_cast<std::size_t>(con->history_index)];
			con->cursor = con->input.size();
		}

		void insert_character(const char ch)
		{
			if (!con || con->input.size() >= max_input_chars)
			{
				return;
			}

			con->input.insert(con->input.begin() + static_cast<std::ptrdiff_t>(con->cursor), ch);
			++con->cursor;
		}

		void handle_special_key(const int vk)
		{
			switch (vk)
			{
			case VK_ESCAPE:
				set_overlay_active(false);
				break;
			case VK_RETURN:
				execute_input();
				break;
			case VK_BACK:
				if (con && con->cursor > 0)
				{
					con->input.erase(con->input.begin() + static_cast<std::ptrdiff_t>(con->cursor - 1));
					--con->cursor;
				}
				break;
			case VK_DELETE:
				if (con && con->cursor < con->input.size())
				{
					con->input.erase(con->input.begin() + static_cast<std::ptrdiff_t>(con->cursor));
				}
				break;
			case VK_LEFT:
				if (con && con->cursor > 0)
				{
					--con->cursor;
				}
				break;
			case VK_RIGHT:
				if (con && con->cursor < con->input.size())
				{
					++con->cursor;
				}
				break;
			case VK_HOME:
				if (con) con->cursor = 0;
				break;
			case VK_END:
				if (con) con->cursor = con->input.size();
				break;
			case VK_UP:
				history_up();
				break;
			case VK_DOWN:
				history_down();
				break;
			default:
				break;
			}
		}

		void handle_text_key(const int vk)
		{
			BYTE keyboard_state[256]{};
			if (!GetKeyboardState(keyboard_state))
			{
				return;
			}

			WCHAR translated[4]{};
			const auto scan_code = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
			const auto result = ToUnicode(static_cast<UINT>(vk), scan_code, keyboard_state, translated, 4, 0);
			if (result != 1)
			{
				return;
			}

			const auto ch = static_cast<char>(translated[0]);
			if (ch >= 32 && ch != 127)
			{
				insert_character(ch);
			}
		}

		void process_console_input()
		{
			if (!overlay_active || !is_game_focused() || !con)
			{
				return;
			}

			for (int vk = 8; vk < 256; ++vk)
			{
				const auto down = is_key_down(vk);
				if (down && !key_was_down[static_cast<std::size_t>(vk)])
				{
					switch (vk)
					{
					case VK_F1:
					case VK_OEM_5:
					case VK_OEM_102:
						break;
					case VK_ESCAPE:
					case VK_RETURN:
					case VK_BACK:
					case VK_DELETE:
					case VK_LEFT:
					case VK_RIGHT:
					case VK_HOME:
					case VK_END:
					case VK_UP:
					case VK_DOWN:
						handle_special_key(vk);
						break;
					default:
						handle_text_key(vk);
						break;
					}
				}

				key_was_down[static_cast<std::size_t>(vk)] = down;
			}
		}
	}

	bool is_active()
	{
		return overlay_active;
	}

	void toggle()
	{
		toggle_overlay_state();
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			if (!con)
			{
				con = new console_state{};
			}

			cl_key_event_hook.create(reinterpret_cast<std::uintptr_t>(game::CL_KeyEvent.get()), cl_key_event_stub);
			cl_console_print_hook.create(reinterpret_cast<std::uintptr_t>(game::CL_ConsolePrint.get()), cl_console_print_stub);
			con_draw_solid_console_hook.create(reinterpret_cast<std::uintptr_t>(game::Con_DrawSolidConsole.get()), con_draw_solid_console_stub);

			scheduler::on_shutdown([]
				{
					set_overlay_active(false);
					key_was_down.fill(false);
				});

			scheduler::loop([]
				{
					if (!is_game_focused())
					{
						was_f1_down = false;
						was_oem5_down = false;
						was_oem102_down = false;
						key_was_down.fill(false);
						return;
					}

					const auto f1_down = is_key_down(VK_F1);
					const auto oem5_down = is_key_down(VK_OEM_5);
					const auto oem102_down = is_key_down(VK_OEM_102);

					if (f1_down && !was_f1_down)
					{
						handle_toggle_press("F1");
					}

					if (oem5_down && !was_oem5_down)
					{
						handle_toggle_press("OEM_5");
					}

					if (oem102_down && !was_oem102_down)
					{
						handle_toggle_press("OEM_102");
					}

					was_f1_down = f1_down;
					was_oem5_down = oem5_down;
					was_oem102_down = oem102_down;
				}, scheduler::main, 50ms);

			scheduler::loop(process_console_input, scheduler::main, 16ms);
		}

		void pre_destroy() override
		{
			process_shutting_down = true;
			set_overlay_active(false);
			cl_key_event_hook.clear();
			cl_console_print_hook.clear();
			con_draw_solid_console_hook.clear();
			con = nullptr;
		}
	};
}

REGISTER_COMPONENT(game_console::component)
