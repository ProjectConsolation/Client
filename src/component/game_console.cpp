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
		utils::hook::detour con_set_console_rect_hook;

		float overlay_back_color[4] = { 0.08f, 0.09f, 0.10f, 0.88f };
		float overlay_border_color[4] = { 0.18f, 0.55f, 0.35f, 1.0f };
		float overlay_text_color[4] = { 0.95f, 0.96f, 0.97f, 1.0f };
		float overlay_hint_color[4] = { 0.65f, 0.75f, 0.70f, 1.0f };

		constexpr auto max_console_lines = 128u;
		constexpr auto max_history_entries = 32u;
		constexpr auto max_input_chars = 240u;
		bool overlay_active = false;
		bool process_shutting_down = false;
		bool render_debug_pending = false;
		bool render_debug_logged = false;
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
		HWND last_render_hwnd = nullptr;
		int last_render_width = 0;
		int last_render_height = 0;
		game::Font_s* last_render_font = nullptr;
		game::Material* last_render_material = nullptr;
		std::size_t last_render_lines = 0;
		float last_scr_scale_virtual_to_real[2] = {};
		float last_scr_scale_virtual_to_full[2] = {};
		float last_scr_scale_real_to_virtual[2] = {};
		float last_scr_virtual_viewable_min[2] = {};
		float last_scr_virtual_viewable_max[2] = {};
		float last_scr_real_viewport_size[2] = {};
		float last_scr_real_viewable_min[2] = {};
		float last_scr_real_viewable_max[2] = {};
		float last_scr_sub_screen[2] = {};
		float last_overlay_view[4] = {};

		void draw_console_overlay();

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

		void con_set_console_rect_stub()
		{
			con_set_console_rect_hook.invoke<void>();

			if (overlay_active)
			{
				render_debug_pending = true;
			}

			draw_console_overlay();
		}

		void debug_log_toggle(const char* key_name)
		{
#ifdef DEBUG
			game::Com_Printf(0,
				"^1debug:^3 game_console.cpp: key=%s pressed, state=%s, visible=%d\n",
				key_name, is_active() ? "open" : "closed", is_active() ? 1 : 0);
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
				render_debug_logged = false;
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

		float scrplace_apply_x(const game::ScreenPlacement& scr_place, const float x, const int horz_align)
		{
			switch (horz_align)
			{
			case 1:
				return (x * scr_place.scaleVirtualToReal[0]) + scr_place.realViewableMin[0];
			case 2:
				return (x * scr_place.scaleVirtualToReal[0]) + (0.5f * scr_place.realViewportSize[0]);
			case 3:
				return (x * scr_place.scaleVirtualToReal[0]) + scr_place.realViewableMax[0];
			case 4:
				return x * scr_place.scaleVirtualToFull[0];
			case 5:
				return x;
			case 6:
				return x * scr_place.scaleRealToVirtual[0];
			case 7:
				return (x * scr_place.scaleVirtualToReal[0]) + ((scr_place.realViewableMin[0] + scr_place.realViewableMax[0]) * 0.5f);
			default:
				return (x * scr_place.scaleVirtualToReal[0]) + scr_place.subScreen[0];
			}
		}

		float scrplace_apply_y(const game::ScreenPlacement& scr_place, const float y, const int vert_align)
		{
			switch (vert_align)
			{
			case 1:
				return (y * scr_place.scaleVirtualToReal[1]) + scr_place.realViewableMin[1];
			case 2:
				return (y * scr_place.scaleVirtualToReal[1]) + (0.5f * scr_place.realViewportSize[1]);
			case 3:
				return (y * scr_place.scaleVirtualToReal[1]) + scr_place.realViewableMax[1];
			case 4:
				return y * scr_place.scaleVirtualToFull[1];
			case 5:
				return y;
			case 6:
				return y * scr_place.scaleRealToVirtual[1];
			case 7:
				return (y * scr_place.scaleVirtualToReal[1]) + ((scr_place.realViewableMin[1] + scr_place.realViewableMax[1]) * 0.5f);
			default:
				return y * scr_place.scaleVirtualToReal[1];
			}
		}

		void scrplace_apply_rect(const game::ScreenPlacement& scr_place, float& x, float& y, float& width, float& height, const int horz_align, const int vert_align)
		{
			switch (horz_align)
			{
			case 1:
				x = (x * scr_place.scaleVirtualToReal[0]) + scr_place.realViewableMin[0];
				width *= scr_place.scaleVirtualToReal[0];
				break;
			case 2:
				x = (x * scr_place.scaleVirtualToReal[0]) + (0.5f * scr_place.realViewportSize[0]);
				width *= scr_place.scaleVirtualToReal[0];
				break;
			case 3:
				x = (x * scr_place.scaleVirtualToReal[0]) + scr_place.realViewableMax[0];
				width *= scr_place.scaleVirtualToReal[0];
				break;
			case 4:
				x *= scr_place.scaleVirtualToFull[0];
				width *= scr_place.scaleVirtualToFull[0];
				break;
			case 5:
				break;
			case 6:
				x *= scr_place.scaleRealToVirtual[0];
				width *= scr_place.scaleRealToVirtual[0];
				break;
			case 7:
				x = (x * scr_place.scaleVirtualToReal[0]) + ((scr_place.realViewableMin[0] + scr_place.realViewableMax[0]) * 0.5f);
				width *= scr_place.scaleVirtualToReal[0];
				break;
			default:
				x = (x * scr_place.scaleVirtualToReal[0]) + scr_place.subScreen[0];
				width *= scr_place.scaleVirtualToReal[0];
				break;
			}

			switch (vert_align)
			{
			case 1:
				y = (y * scr_place.scaleVirtualToReal[1]) + scr_place.realViewableMin[1];
				height *= scr_place.scaleVirtualToReal[1];
				break;
			case 2:
				y = (y * scr_place.scaleVirtualToReal[1]) + (0.5f * scr_place.realViewportSize[1]);
				height *= scr_place.scaleVirtualToReal[1];
				break;
			case 3:
				y = (y * scr_place.scaleVirtualToReal[1]) + scr_place.realViewableMax[1];
				height *= scr_place.scaleVirtualToReal[1];
				break;
			case 4:
				y *= scr_place.scaleVirtualToFull[1];
				height *= scr_place.scaleVirtualToFull[1];
				break;
			case 5:
				break;
			case 6:
				y *= scr_place.scaleRealToVirtual[1];
				height *= scr_place.scaleRealToVirtual[1];
				break;
			case 7:
				y = (y * scr_place.scaleVirtualToReal[1]) + ((scr_place.realViewableMin[1] + scr_place.realViewableMax[1]) * 0.5f);
				height *= scr_place.scaleVirtualToReal[1];
				break;
			default:
				y *= scr_place.scaleVirtualToReal[1];
				height *= scr_place.scaleVirtualToReal[1];
				break;
			}
		}

		overlay_view get_overlay_view()
		{
			return { 6.0f, 6.0f, 628.0f, 276.0f };
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

			if (render_debug_pending)
			{
				const auto hwnd = get_window();
				RECT client_rect{};
				if (hwnd)
				{
					GetClientRect(hwnd, &client_rect);
				}

				last_render_hwnd = hwnd;
				last_render_width = client_rect.right - client_rect.left;
				last_render_height = client_rect.bottom - client_rect.top;
				last_render_font = get_console_font();
				last_render_material = get_white_material();
				last_render_lines = con->lines.size();
				const auto scr_place = game::ScrPlace_GetViewPlacement();
				last_scr_scale_virtual_to_real[0] = scr_place.scaleVirtualToReal[0];
				last_scr_scale_virtual_to_real[1] = scr_place.scaleVirtualToReal[1];
				last_scr_scale_virtual_to_full[0] = scr_place.scaleVirtualToFull[0];
				last_scr_scale_virtual_to_full[1] = scr_place.scaleVirtualToFull[1];
				last_scr_scale_real_to_virtual[0] = scr_place.scaleRealToVirtual[0];
				last_scr_scale_real_to_virtual[1] = scr_place.scaleRealToVirtual[1];
				last_scr_virtual_viewable_min[0] = scr_place.virtualViewableMin[0];
				last_scr_virtual_viewable_min[1] = scr_place.virtualViewableMin[1];
				last_scr_virtual_viewable_max[0] = scr_place.virtualViewableMax[0];
				last_scr_virtual_viewable_max[1] = scr_place.virtualViewableMax[1];
				last_scr_real_viewport_size[0] = scr_place.realViewportSize[0];
				last_scr_real_viewport_size[1] = scr_place.realViewportSize[1];
				last_scr_real_viewable_min[0] = scr_place.realViewableMin[0];
				last_scr_real_viewable_min[1] = scr_place.realViewableMin[1];
				last_scr_real_viewable_max[0] = scr_place.realViewableMax[0];
				last_scr_real_viewable_max[1] = scr_place.realViewableMax[1];
				last_scr_sub_screen[0] = scr_place.subScreen[0];
				last_scr_sub_screen[1] = scr_place.subScreen[1];
				render_debug_pending = false;
			}

			const auto view = get_overlay_view();
			if (view.width <= 0.0f || view.height <= 0.0f)
			{
				return;
			}
			last_overlay_view[0] = view.left;
			last_overlay_view[1] = view.top;
			last_overlay_view[2] = view.width;
			last_overlay_view[3] = view.height;

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
			con_set_console_rect_hook.create(reinterpret_cast<std::uintptr_t>(game::Con_SetConsoleRect.get()), con_set_console_rect_stub);

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

					if (overlay_active && !render_debug_pending && !render_debug_logged && last_render_hwnd)
					{
#ifdef DEBUG
						game::Com_Printf(0,
							"^1debug:^3 game_console.cpp: render active hwnd=%p size=%dx%d font=%p material=%p lines=%zu overlay=(%.1f,%.1f %.1fx%.1f) "
							"scr_v2r=(%.3f,%.3f) scr_v2f=(%.3f,%.3f) scr_r2v=(%.3f,%.3f) "
							"vmin=(%.1f,%.1f) vmax=(%.1f,%.1f) rvp=(%.1f,%.1f) rmin=(%.1f,%.1f) rmax=(%.1f,%.1f) sub=%.1f\n",
							last_render_hwnd,
							last_render_width,
							last_render_height,
							last_render_font,
							last_render_material,
							last_render_lines,
							last_overlay_view[0],
							last_overlay_view[1],
							last_overlay_view[2],
							last_overlay_view[3],
							last_scr_scale_virtual_to_real[0],
							last_scr_scale_virtual_to_real[1],
							last_scr_scale_virtual_to_full[0],
							last_scr_scale_virtual_to_full[1],
							last_scr_scale_real_to_virtual[0],
							last_scr_scale_real_to_virtual[1],
							last_scr_virtual_viewable_min[0],
							last_scr_virtual_viewable_min[1],
							last_scr_virtual_viewable_max[0],
							last_scr_virtual_viewable_max[1],
							last_scr_real_viewport_size[0],
							last_scr_real_viewport_size[1],
							last_scr_real_viewable_min[0],
							last_scr_real_viewable_min[1],
							last_scr_real_viewable_max[0],
							last_scr_real_viewable_max[1],
							last_scr_sub_screen[0]);
#endif
						render_debug_logged = true;
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
			con_set_console_rect_hook.clear();
			last_render_hwnd = nullptr;
			con = nullptr;
		}
	};
}

REGISTER_COMPONENT(game_console::component)
