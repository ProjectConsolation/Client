#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"
#include "component/engine/console/game_console.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <deque>
#include <mutex>
#include <unordered_set>

#ifndef VERSION_BUILD
#define VERSION_BUILD "0"
#endif

namespace game_console
{
	namespace
	{
		utils::hook::detour cl_key_event_hook;
		utils::hook::detour cl_console_print_hook;
		utils::hook::detour con_set_console_rect_hook;

		float overlay_back_color[4] = { 0.01f, 0.01f, 0.01f, 0.82f };
		float overlay_panel_color[4] = { 0.03f, 0.03f, 0.03f, 0.90f };
		float overlay_header_color[4] = { 0.07f, 0.02f, 0.02f, 0.98f };
		float overlay_border_color[4] = { 0.46f, 0.10f, 0.10f, 0.95f };
		float overlay_accent_color[4] = { 0.74f, 0.13f, 0.13f, 0.95f };
		float overlay_text_color[4] = { 0.95f, 0.95f, 0.95f, 1.0f };
		float overlay_hint_color[4] = { 0.78f, 0.62f, 0.62f, 1.0f };
		float overlay_selected_color[4] = { 0.16f, 0.05f, 0.05f, 0.95f };
		float overlay_match_color[4] = { 0.90f, 0.82f, 0.82f, 1.0f };
		float overlay_shadow_color[4] = { 0.0f, 0.0f, 0.0f, 0.65f };

		constexpr auto max_console_lines = 128u;
		constexpr auto max_history_entries = 32u;
		constexpr auto max_input_chars = 240u;
		constexpr auto max_auto_complete_matches = 24u;
		bool overlay_active = false;
		bool process_shutting_down = false;
		bool component_ready = false;
		bool hooks_installed = false;
		bool was_f1_down = false;
		bool was_oem5_down = false;
		bool was_oem102_down = false;
		std::array<bool, 256> key_was_down{};
		std::array<bool, 256> key_is_down{};
		std::array<DWORD, 256> key_next_repeat_time{};
		std::vector<std::string> cached_command_names{};
		std::vector<std::string> cached_dvar_names{};
		game::dvar_s* con_outputHeightScale = nullptr;

		struct console_state
		{
			std::string input{};
			std::size_t cursor = 0;
			std::size_t selection_start = 0;
			std::size_t selection_end = 0;
			int history_index = -1;
			int scroll_offset = 0;
			bool output_visible = false;
			bool output_fullscreen = false;
			std::vector<std::string> history{};
			std::vector<std::string> lines{};
			std::vector<std::string> auto_complete_matches{};
			std::string auto_complete_query{};
			std::string auto_complete_choice{};
			bool may_auto_complete = false;
		};

		console_state* con = nullptr;

		std::mutex& get_pending_output_mutex()
		{
			static auto* mutex = new std::mutex{};
			return *mutex;
		}

		std::deque<std::string>& get_pending_output_queue()
		{
			static auto* queue = new std::deque<std::string>{};
			return *queue;
		}

		float color_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float color_qos[4] = { 0.85f, 0.15f, 0.15f, 1.0f };
		float color_input_box[4] = { 0.20f, 0.20f, 0.20f, 0.90f };
		float color_hint_box[4] = { 0.30f, 0.30f, 0.30f, 1.0f };
		float color_output_bar[4] = { 0.50f, 0.50f, 0.50f, 0.60f };
		float color_output_slider[4] = { 0.85f, 0.15f, 0.15f, 1.0f };
		float color_output_window[4] = { 0.25f, 0.25f, 0.25f, 0.85f };
		float color_dvar_match[4] = { 1.0f, 1.0f, 0.8f, 1.0f };
		float color_dvar_value[4] = { 1.0f, 1.0f, 0.8f, 1.0f };
		float color_dvar_inactive[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
		float color_cmd_match[4] = { 0.72f, 0.84f, 1.0f, 1.0f };
		float color_version_footer[4] = { 0.70f, 0.22f, 0.22f, 1.0f };

		HWND get_window();
		void draw_console_overlay();
		void insert_character(char ch);
		void insert_text(std::string text);
		void set_cursor_position(std::size_t cursor);
		void refresh_auto_complete();
		void clear_dead_key_state();
		void cl_key_event_stub();
		void cl_console_print_stub(int local_client_num, int channel, const char* txt, int duration, int pixel_width, int flags);
		void con_set_console_rect_stub();

		void install_hooks_if_ready()
		{
			if (hooks_installed || process_shutting_down)
			{
				return;
			}

			const auto hwnd = get_window();
			if (!hwnd || !IsWindow(hwnd))
			{
				return;
			}

			cl_key_event_hook.create(reinterpret_cast<std::uintptr_t>(game::CL_KeyEvent_.get()), cl_key_event_stub);
			cl_console_print_hook.create(reinterpret_cast<std::uintptr_t>(game::CL_ConsolePrint.get()), cl_console_print_stub);
			con_set_console_rect_hook.create(reinterpret_cast<std::uintptr_t>(game::Con_SetConsoleRect.get()), con_set_console_rect_stub);
			hooks_installed = true;
			component_ready = true;
		}

		std::string build_display_version()
		{
			std::string short_hash = GIT_HASH;
			if (short_hash.size() > 7)
			{
				short_hash.resize(7);
			}

			std::string version = std::string(VERSION_PRODUCT) + "(" + VERSION_BUILD + ")";

#ifdef DEBUG
			version += "-dbg-";
#else
			version += "-nightly-";
#endif

			version += short_hash;

			if (GIT_DIRTY)
			{
				version += "-dirty";
			}

			return version;
		}

		std::string build_prompt_version()
		{
			std::string version = VERSION_PRODUCT;

#ifdef DEBUG
			version += "-dbg";
#elif defined(NDEBUG)
			// release keeps the plain semantic version
#else
			version += "-nightly";
#endif

			std::string short_hash = GIT_HASH;
			if (short_hash.size() > 7)
			{
				short_hash.resize(7);
			}

			std::string hash_label = short_hash;
			if (GIT_DIRTY)
			{
				hash_label += "-dirty";
			}

			return version + " [" + hash_label + "]";
		}

		std::string build_full_version_string()
		{
			std::string version = "Project: Consolation ";
			version += VERSION_PRODUCT;

#ifdef DEBUG
			version += "-dbg";
#elif defined(NDEBUG)
			// release keeps the plain semantic version
#else
			version += "-nightly";
#endif

			std::string short_hash = GIT_HASH;
			if (short_hash.size() > 7)
			{
				short_hash.resize(7);
			}

			version += " build ";
			version += short_hash;

			if (GIT_DIRTY)
			{
				version += "-dirty";
			}

			version += " ";
			version += __DATE__;
			version += " ";
			version += __TIME__;
			version += " win-x86";

			return version;
		}

		std::string build_console_prompt()
		{
			return "Project: Consolation " + build_prompt_version() + " >";
		}

		std::string to_lower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
				{
					return static_cast<char>(std::tolower(c));
				});

			return value;
		}

		bool is_reasonable_console_token(std::string_view value)
		{
			if (value.empty())
			{
				return false;
			}

			for (const auto ch : value)
			{
				const auto uch = static_cast<unsigned char>(ch);
				if (std::isalnum(uch))
				{
					continue;
				}

				switch (ch)
				{
				case '_':
				case '-':
				case '+':
				case '.':
				case ':':
				case '/':
				case '\\':
				case '*':
				case '?':
				case '$':
				case '@':
					continue;
				default:
					return false;
				}
			}

			return true;
		}

		game::Font_s* get_console_font()
		{
			if (game::con_font && *game::con_font)
			{
				return *game::con_font;
			}

			return game::R_RegisterFont("fonts/normalFont");
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

		bool is_modifier_down(const int vk, const int left_vk, const int right_vk)
		{
			return key_is_down[static_cast<std::size_t>(vk)]
				|| key_is_down[static_cast<std::size_t>(left_vk)]
				|| key_is_down[static_cast<std::size_t>(right_vk)]
				|| is_key_down(vk)
				|| is_key_down(left_vk)
				|| is_key_down(right_vk);
		}

		bool is_any_shift_down()
		{
			return is_modifier_down(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
		}

		bool is_any_ctrl_down()
		{
			return is_modifier_down(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
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

			if (con->scroll_offset < 0)
			{
				con->scroll_offset = 0;
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

		std::string strip_carriage_returns(std::string_view text)
		{
			std::string cleaned{};
			cleaned.reserve(text.size());

			for (const auto ch : text)
			{
				if (ch != '\r')
				{
					cleaned.push_back(ch);
				}
			}

			return cleaned;
		}

		std::string sanitize_display_text(std::string_view text)
		{
			auto cleaned = strip_colors(text);
			cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(), [](const unsigned char c)
				{
					return c < 32 || c > 126;
				}), cleaned.end());
			return cleaned;
		}

		void append_line(const std::string& line)
		{
			if (!con)
			{
				return;
			}

			if (!con->lines.empty() && con->lines.back() == line)
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

			const auto cleaned = strip_carriage_returns(text);
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

		void flush_pending_output()
		{
			if (!component_ready || process_shutting_down || !con)
			{
				return;
			}

			std::deque<std::string> queued{};
			{
				auto& pending_output = get_pending_output_queue();
				std::lock_guard _(get_pending_output_mutex());
				if (pending_output.empty())
				{
					return;
				}

				queued.swap(pending_output);
			}

			for (const auto& line : queued)
			{
				append_console_text(line.c_str());
			}
		}

		bool try_copy_c_string(const char* text, std::string& out)
		{
			out.clear();

			char buffer[512]{};
			__try
			{
				if (!text || !*text)
				{
					return false;
				}

				std::size_t index = 0;
				for (; index + 1 < sizeof(buffer); ++index)
				{
					const auto ch = text[index];
					buffer[index] = ch;
					if (ch == '\0')
					{
						break;
					}
				}

				buffer[sizeof(buffer) - 1] = '\0';
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}

			if (!buffer[0])
			{
				return false;
			}

			out = buffer;
			return true;
		}

		bool try_get_dvar_value_ptr(game::dvar_s* dvar, const game::DvarValue value, const char** value_out)
		{
			if (value_out)
			{
				*value_out = nullptr;
			}

			if (!dvar)
			{
				return false;
			}

			__try
			{
				if (value_out)
				{
					*value_out = dvars::Dvar_ValueToString(dvar, value);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}

			return value_out && *value_out != nullptr;
		}

		bool try_copy_dvar_domain(game::dvar_s* dvar, game::dvar_type& type_out, game::DvarLimits& domain_out)
		{
			if (!dvar)
			{
				return false;
			}

			__try
			{
				type_out = dvar->type;
				domain_out = dvar->domain;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}

			return true;
		}

		bool safe_read_cmd_name(game::cmd_function_s* current, game::cmd_function_s** next_out, char* name, const std::size_t name_size)
		{
			if (next_out)
			{
				*next_out = nullptr;
			}

			if (name_size)
			{
				name[0] = '\0';
			}

			__try
			{
				if (next_out)
				{
					*next_out = current ? current->next : nullptr;
				}

				if (!current || !current->name || !current->name[0])
				{
					return false;
				}

				if (!name_size)
				{
					return true;
				}

				std::size_t index = 0;
				for (; index + 1 < name_size; ++index)
				{
					const auto ch = current->name[index];
					name[index] = ch;
					if (ch == '\0')
					{
						return index > 0;
					}
				}

				name[name_size - 1] = '\0';
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (next_out)
				{
					*next_out = nullptr;
				}
				if (name_size)
				{
					name[0] = '\0';
				}
				return false;
			}
		}

		bool safe_read_dvar_name(game::dvar_s* current, char* name, const std::size_t name_size)
		{
			if (name_size)
			{
				name[0] = '\0';
			}

			__try
			{
				if (!current || !current->name || !current->name[0])
				{
					return false;
				}

				if (!name_size)
				{
					return true;
				}

				std::size_t index = 0;
				for (; index + 1 < name_size; ++index)
				{
					const auto ch = current->name[index];
					name[index] = ch;
					if (ch == '\0')
					{
						return index > 0;
					}
				}

				name[name_size - 1] = '\0';
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (name_size)
				{
					name[0] = '\0';
				}
				return false;
			}
		}

		bool safe_read_dvar_description(game::dvar_s* current, char* description, const std::size_t description_size)
		{
			if (description_size)
			{
				description[0] = '\0';
			}

			__try
			{
				if (!current || !current->description || !current->description[0])
				{
					return false;
				}

				if (!description_size)
				{
					return true;
				}

				std::size_t index = 0;
				for (; index + 1 < description_size; ++index)
				{
					const auto ch = current->description[index];
					description[index] = ch;
					if (ch == '\0')
					{
						return index > 0;
					}
				}

				description[description_size - 1] = '\0';
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (description_size)
				{
					description[0] = '\0';
				}
				return false;
			}
		}

		bool safe_read_dvar_hash_entry(game::dvar_s* current, game::dvar_s** next_out, char* name, const std::size_t name_size)
		{
			if (next_out)
			{
				*next_out = nullptr;
			}

			if (name_size)
			{
				name[0] = '\0';
			}

			__try
			{
				if (next_out)
				{
					*next_out = current ? current->hashNext : nullptr;
				}

				if (!current || !current->name || !current->name[0])
				{
					return false;
				}

				if (!name_size)
				{
					return true;
				}

				std::size_t index = 0;
				for (; index + 1 < name_size; ++index)
				{
					const auto ch = current->name[index];
					name[index] = ch;
					if (ch == '\0')
					{
						return index > 0;
					}
				}

				name[name_size - 1] = '\0';
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (next_out)
				{
					*next_out = nullptr;
				}
				if (name_size)
				{
					name[0] = '\0';
				}
				return false;
			}
		}

		void rebuild_auto_complete_cache()
		{
			cached_command_names.clear();
			cached_dvar_names.clear();

			std::unordered_set<std::string> seen_commands{};
			std::unordered_set<std::string> seen_dvars{};

			auto* cmd = *game::cmd_functions;
			for (auto cmd_count = 0; cmd && cmd_count < 4096; ++cmd_count)
			{
				char name_buffer[256]{};
				auto* next = cmd;
				if (!safe_read_cmd_name(cmd, &next, name_buffer, sizeof(name_buffer)))
				{
					cmd = next;
					continue;
				}

				cmd = next;
				std::string name = name_buffer;
				const auto folded_name = to_lower(name);
				if (is_reasonable_console_token(name) && seen_commands.insert(folded_name).second)
				{
					cached_command_names.emplace_back(std::move(name));
				}
			}

			for (auto bucket = 0; bucket < 256; ++bucket)
			{
				auto* dvar = game::dvarHashTable[bucket];
				for (auto dvar_count = 0; dvar && dvar_count < 16384; ++dvar_count)
				{
					char name_buffer[256]{};
					auto* next = dvar;
					if (!safe_read_dvar_hash_entry(dvar, &next, name_buffer, sizeof(name_buffer)))
					{
						dvar = next;
						continue;
					}

					dvar = next;
					std::string name = name_buffer;
					const auto folded_name = to_lower(name);
					if (is_reasonable_console_token(name) && seen_dvars.insert(folded_name).second)
					{
						cached_dvar_names.emplace_back(std::move(name));
					}
				}
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
			draw_console_overlay();
		}

		void debug_log_toggle(const char* key_name)
		{
#ifdef DEBUG
			/*game::Com_Printf(0,
				"^1debug:^3 game_console.cpp: key=%s pressed, state=%s, visible=%d\n",
				key_name, is_active() ? "open" : "closed", is_active() ? 1 : 0);*/
			(void)key_name;
#else
			(void)key_name;
#endif
		}

		void set_overlay_active(const bool active)
		{
			overlay_active = active;
			key_was_down.fill(false);
			key_is_down.fill(false);
			key_next_repeat_time.fill(0);

			if (!con)
			{
				return;
			}

			con->auto_complete_matches.clear();
			con->auto_complete_query.clear();
			con->auto_complete_choice.clear();
			con->may_auto_complete = false;

			if (overlay_active)
			{
				con->history_index = -1;
				con->scroll_offset = 0;
				con->output_visible = false;
				con->output_fullscreen = false;
			}
			else
			{
				con->input.clear();
				set_cursor_position(0);
				con->history_index = -1;
				con->scroll_offset = 0;
				con->output_visible = false;
				con->output_fullscreen = false;
			}
		}

		void clear_auto_complete()
		{
			if (!con)
			{
				return;
			}

			con->auto_complete_matches.clear();
			con->auto_complete_query.clear();
			con->auto_complete_choice.clear();
			con->may_auto_complete = false;
		}

		void clear_input_line()
		{
			if (!con)
			{
				return;
			}

			con->input.clear();
			set_cursor_position(0);
			clear_auto_complete();
		}

		void scroll_output(const int amount)
		{
			if (!con || !con->output_visible || amount == 0)
			{
				return;
			}

			const bool match_view = !con->auto_complete_query.empty() && !con->auto_complete_matches.empty();
			if (match_view)
			{
				con->scroll_offset = std::max(0, con->scroll_offset - amount);
			}
			else
			{
				con->scroll_offset = std::max(0, con->scroll_offset + amount);
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

		void handle_full_console_toggle_press(const char* key_name)
		{
			if (!overlay_active)
			{
				set_overlay_active(true);
			}

			if (con)
			{
				const bool show_full_console = !(con->output_visible && con->output_fullscreen);
				con->output_visible = show_full_console;
				con->output_fullscreen = show_full_console;
				con->scroll_offset = 0;
				refresh_auto_complete();
			}

			debug_log_toggle(key_name);
		}

		struct overlay_bounds
		{
			float screen_min[2];
			float screen_max[2];
			float x;
			float y;
			float left_x;
			float font_height;
			int visible_line_count;
			int visible_pixel_width;
		};

		float resolve_layout_width(const game::ScreenPlacement& scr_place)
		{
			const auto real_a = scr_place.realViewportSize[0];
			const auto real_b = scr_place.realViewportSize[1];
			const auto virtual_a = scr_place.virtualViewableMax[0];
			const auto virtual_b = scr_place.virtualViewableMax[1];

			if (real_a > 0.0f && real_b > 0.0f)
			{
				return std::max(real_a, real_b);
			}

			if (virtual_a > 0.0f && virtual_b > 0.0f)
			{
				return std::max(virtual_a, virtual_b);
			}

			return 640.0f;
		}

		float resolve_layout_height(const game::ScreenPlacement& scr_place)
		{
			const auto real_a = scr_place.realViewportSize[0];
			const auto real_b = scr_place.realViewportSize[1];
			const auto virtual_a = scr_place.virtualViewableMax[0];
			const auto virtual_b = scr_place.virtualViewableMax[1];

			if (real_a > 0.0f && real_b > 0.0f)
			{
				return std::min(real_a, real_b);
			}

			if (virtual_a > 0.0f && virtual_b > 0.0f)
			{
				return std::min(virtual_a, virtual_b);
			}

			return 480.0f;
		}

		overlay_bounds get_overlay_bounds()
		{
			overlay_bounds bounds{};
			bounds.screen_min[0] = 6.0f;
			bounds.screen_min[1] = 6.0f;

			float layout_width = 0.0f;
			float layout_height = 0.0f;

			RECT client_rect{};
			const auto hwnd = get_window();
			if (hwnd && GetClientRect(hwnd, &client_rect))
			{
				layout_width = static_cast<float>(client_rect.right - client_rect.left);
				layout_height = static_cast<float>(client_rect.bottom - client_rect.top);
			}
			else
			{
				const auto scr_place = game::ScrPlace_GetViewPlacement();
				layout_width = resolve_layout_width(scr_place);
				layout_height = resolve_layout_height(scr_place);
			}

			bounds.screen_max[0] = std::max(634.0f, layout_width - 6.0f);
			bounds.screen_max[1] = std::max(474.0f, layout_height - 6.0f);
			bounds.font_height = static_cast<float>(get_console_font() ? get_console_font()->pixelHeight : 0);
			bounds.x = bounds.screen_min[0] + 6.0f;
			bounds.y = bounds.screen_min[1] + 6.0f;
			bounds.left_x = bounds.x;

			if (bounds.font_height > 0.0f)
			{
				bounds.visible_line_count = static_cast<int>((bounds.screen_max[1] - bounds.screen_min[1] - (bounds.font_height * 2.0f) - 24.0f) / bounds.font_height);
				bounds.visible_pixel_width = static_cast<int>(((bounds.screen_max[0] - bounds.screen_min[0]) - 10.0f) - 18.0f);
			}

			return bounds;
		}

		std::vector<std::string> gather_auto_complete_matches(std::string input, const bool exact)
		{
			if (input.empty())
			{
				return {};
			}

			input = to_lower(std::move(input));
			std::vector<std::string> exact_matches{};
			std::vector<std::string> prefix_matches{};
			std::vector<std::string> contains_matches{};
			std::unordered_set<std::string> exact_seen{};
			std::unordered_set<std::string> prefix_seen{};
			std::unordered_set<std::string> contains_seen{};

			auto add_candidate = [&](const std::string& candidate)
				{
					auto name = to_lower(candidate);

					if (exact)
					{
						if (name == input && exact_seen.insert(name).second)
						{
							exact_matches.emplace_back(candidate);
						}
						return;
					}

					if (name == input)
					{
						if (exact_seen.insert(name).second)
						{
							exact_matches.emplace_back(candidate);
						}
					}
					else if (name.rfind(input, 0) == 0)
					{
						if (prefix_seen.insert(name).second)
						{
							prefix_matches.emplace_back(candidate);
						}
					}
					else if (name.find(input) != std::string::npos)
					{
						if (contains_seen.insert(name).second)
						{
							contains_matches.emplace_back(candidate);
						}
					}
				};

			for (const auto& name : cached_command_names)
			{
				add_candidate(name);
			}

			for (const auto& name : cached_dvar_names)
			{
				add_candidate(name);
			}

			auto sort_matches = [](std::vector<std::string>& matches)
			{
				std::sort(matches.begin(), matches.end(), [](const std::string& lhs, const std::string& rhs)
					{
						return _stricmp(lhs.c_str(), rhs.c_str()) < 0;
					});
			};

			if (!exact_matches.empty())
			{
				sort_matches(exact_matches);
				return exact_matches;
			}

			if (!prefix_matches.empty())
			{
				sort_matches(prefix_matches);
				return prefix_matches;
			}

			sort_matches(contains_matches);
			return contains_matches;
		}

		void refresh_auto_complete()
		{
			if (!con)
			{
				return;
			}

			if (cached_command_names.empty() && cached_dvar_names.empty())
			{
				rebuild_auto_complete_cache();
				if (cached_command_names.empty() && cached_dvar_names.empty())
				{
					clear_auto_complete();
					return;
				}
			}

			auto input = con->input;
			if (input.size() > 1 && (input[0] == '/' || input[0] == '\\'))
			{
				input = input.substr(1);
			}

			if (input.empty())
			{
				clear_auto_complete();
				return;
			}

			const auto separator = input.find(' ');
			const auto query = separator == std::string::npos ? input : input.substr(0, separator);
			const auto exact = separator != std::string::npos;

			con->auto_complete_query = query;
			con->auto_complete_matches = gather_auto_complete_matches(query, exact);
			if (con->auto_complete_matches.empty())
			{
				rebuild_auto_complete_cache();
				con->auto_complete_matches = gather_auto_complete_matches(query, exact);
			}
			con->auto_complete_choice = con->auto_complete_matches.empty() ? std::string{} : con->auto_complete_matches.front();
			con->may_auto_complete = !con->auto_complete_choice.empty();
		}

		void handle_auto_complete()
		{
			if (!con)
			{
				return;
			}

			if (con->input.empty())
			{
				clear_auto_complete();
				return;
			}

			if (cached_command_names.empty() && cached_dvar_names.empty())
			{
				rebuild_auto_complete_cache();
			}

			refresh_auto_complete();
			if (!con->may_auto_complete || con->auto_complete_choice.empty())
			{
				return;
			}

			const auto first_char = con->input.front();
			const bool has_prefix = first_char == '\\' || first_char == '/';
			con->input.clear();
			set_cursor_position(0);

			if (has_prefix)
			{
				con->input.push_back(first_char);
				set_cursor_position(1);
			}

			insert_text(con->auto_complete_choice);
			if (con->cursor < max_input_chars)
			{
				insert_character(' ');
			}
		}

		void draw_box(const float x, const float y, const float width, const float height, float* color)
		{
			auto* const material = get_white_material();
			if (!material)
			{
				return;
			}

			float dark_color[4]{};
			dark_color[0] = color[0] * 0.5f;
			dark_color[1] = color[1] * 0.5f;
			dark_color[2] = color[2] * 0.5f;
			dark_color[3] = color[3];

			game::R_AddCmdDrawStretchPic(x, y, width, height, 0.0f, 0.0f, 0.0f, 0.0f, color, material, 0);
			game::R_AddCmdDrawStretchPic(x, y, 2.0f, height, 0.0f, 0.0f, 0.0f, 0.0f, dark_color, material, 0);
			game::R_AddCmdDrawStretchPic((x + width) - 2.0f, y, 2.0f, height, 0.0f, 0.0f, 0.0f, 0.0f, dark_color, material, 0);
			game::R_AddCmdDrawStretchPic(x, y, width, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, dark_color, material, 0);
			game::R_AddCmdDrawStretchPic(x, (y + height) - 2.0f, width, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, dark_color, material, 0);
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

		void draw_text_shadowed(const char* text, float x, float y, float* color, float scale = 1.0f)
		{
			draw_text(text, x + 1.0f, y + 1.0f, overlay_shadow_color, scale);
			draw_text(text, x, y, color, scale);
		}

		void insert_wide_text(const WCHAR* text, const int length)
		{
			if (!text || length <= 0)
			{
				return;
			}

			for (int i = 0; i < length; ++i)
			{
				if (!text[i])
				{
					break;
				}

				char converted[8]{};
				const auto converted_length = WideCharToMultiByte(
					CP_ACP, 0, &text[i], 1, converted, static_cast<int>(sizeof(converted)), nullptr, nullptr);
				if (converted_length <= 0)
				{
					continue;
				}

				for (int j = 0; j < converted_length; ++j)
				{
					const auto ch = static_cast<unsigned char>(converted[j]);
					if (ch >= 32 && ch != 127)
					{
						insert_character(static_cast<char>(ch));
					}
				}
			}
		}

		void clear_dead_key_state()
		{
			BYTE empty_keyboard_state[256]{};
			WCHAR clear_buffer[8]{};
			ToUnicode(VK_SPACE, MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC), empty_keyboard_state, clear_buffer, 8, 0);
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

		float get_text_width(std::string_view text)
		{
			auto* const font = get_console_font();
			if (!font || text.empty())
			{
				return 0.0f;
			}

			const std::string owned_text{text};
			return static_cast<float>(game::R_TextWidth(owned_text.c_str(), 0x7FFFFFFF, font));
		}

		bool has_input_selection()
		{
			return con && con->selection_start != con->selection_end;
		}

		std::size_t get_selection_begin()
		{
			return con ? std::min(con->selection_start, con->selection_end) : 0;
		}

		std::size_t get_selection_end()
		{
			return con ? std::max(con->selection_start, con->selection_end) : 0;
		}

		void clear_input_selection()
		{
			if (!con)
			{
				return;
			}

			con->selection_start = con->cursor;
			con->selection_end = con->cursor;
		}

		void set_cursor_position(const std::size_t cursor)
		{
			if (!con)
			{
				return;
			}

			con->cursor = std::min(cursor, con->input.size());
			clear_input_selection();
		}

		void select_entire_input()
		{
			if (!con)
			{
				return;
			}

			con->selection_start = 0;
			con->selection_end = con->input.size();
			con->cursor = con->input.size();
		}

		bool delete_input_selection()
		{
			if (!con || !has_input_selection())
			{
				return false;
			}

			const auto selection_begin = get_selection_begin();
			const auto selection_end = get_selection_end();
			con->input.erase(selection_begin, selection_end - selection_begin);
			con->cursor = selection_begin;
			clear_input_selection();
			return true;
		}

		void copy_text_to_clipboard(std::string_view text)
		{
			if (!OpenClipboard(nullptr))
			{
				return;
			}

			if (!EmptyClipboard())
			{
				CloseClipboard();
				return;
			}

			const auto allocation_size = text.size() + 1;
			auto clipboard_memory = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
			if (!clipboard_memory)
			{
				CloseClipboard();
				return;
			}

			auto* const clipboard_buffer = static_cast<char*>(GlobalLock(clipboard_memory));
			if (!clipboard_buffer)
			{
				GlobalFree(clipboard_memory);
				CloseClipboard();
				return;
			}

			memcpy(clipboard_buffer, text.data(), text.size());
			clipboard_buffer[text.size()] = '\0';
			GlobalUnlock(clipboard_memory);

			if (!SetClipboardData(CF_TEXT, clipboard_memory))
			{
				GlobalFree(clipboard_memory);
			}

			CloseClipboard();
		}

		void draw_input_box(const overlay_bounds& bounds, const int lines, float* color)
		{
			draw_box(
				bounds.x - 6.0f,
				bounds.y - 6.0f,
				(bounds.screen_max[0] - bounds.screen_min[0]) - ((bounds.x - 6.0f) - bounds.screen_min[0]),
				(lines * bounds.font_height) + 12.0f,
				color);
		}

		void draw_hint_box(const overlay_bounds& bounds, const float hint_x, const int lines, float* color, const float offset_y = 0.0f)
		{
			const auto height = lines * bounds.font_height + 12.0f;
			const auto y = bounds.y - 3.0f + bounds.font_height + 12.0f + offset_y;
			const auto width = (bounds.screen_max[0] - bounds.screen_min[0]) - ((hint_x - 6.0f) - bounds.screen_min[0]);
			draw_box(hint_x - 6.0f, y, width, height, color);
		}

		void draw_hint_text(const overlay_bounds& bounds, const float hint_x, const int line, const char* text, float* color, const float offset = 0.0f, const float offset_y = 0.0f)
		{
			const auto y = bounds.font_height + bounds.y + (bounds.font_height * (line + 1)) + 15.0f + offset_y;
			draw_text(text, hint_x + offset, y, color, 1.0f);
		}

		void draw_input(const overlay_bounds& bounds)
		{
			if (!con)
			{
				return;
			}

			draw_input_box(bounds, 1, color_input_box);

			float draw_x = bounds.x;
			const auto prompt_prefix = build_console_prompt();
			const auto input_y = bounds.y + bounds.font_height;
			draw_text(prompt_prefix.c_str(), draw_x, input_y, color_qos, 1.0f);
			draw_x += static_cast<float>(game::R_TextWidth(prompt_prefix.c_str(), 0x7FFFFFFF, get_console_font())) + 6.0f;
			const auto hint_x = draw_x;
			const auto cursor_position = std::min(con->cursor, con->input.size());

			if (has_input_selection())
			{
				const auto selection_begin = std::min(get_selection_begin(), con->input.size());
				const auto selection_end = std::min(get_selection_end(), con->input.size());
				const auto before_selection_width = get_text_width(std::string_view(con->input).substr(0, selection_begin));
				const auto selection_width = get_text_width(std::string_view(con->input).substr(selection_begin, selection_end - selection_begin));
				const auto selection_x = draw_x + before_selection_width;
				draw_box(selection_x - 1.0f, bounds.y - 2.0f, selection_width + 2.0f, bounds.font_height + 6.0f, overlay_selected_color);
			}

			draw_text(con->input.c_str(), draw_x, input_y, color_white, 1.0f);

			const auto blink_on = ((GetTickCount() / 500u) & 1u) == 0u;
			if (blink_on)
			{
				const auto cursor_x = draw_x + get_text_width(std::string_view(con->input).substr(0, cursor_position));
				draw_text("|", cursor_x, input_y, color_white, 1.0f);
			}

			if (con->output_visible)
			{
				return;
			}

			if (con->auto_complete_query.empty() || con->auto_complete_matches.empty())
			{
				return;
			}

			if (con->auto_complete_matches.size() > max_auto_complete_matches)
			{
				draw_hint_box(bounds, hint_x, 1, color_hint_box);
				draw_hint_text(
					bounds,
					hint_x,
					0,
					utils::string::va(
						"%i matches (too many to show here, press shift+tilde to open full console)",
						static_cast<int>(con->auto_complete_matches.size())),
					color_dvar_match);
				return;
			}

			if (con->auto_complete_matches.size() == 1)
			{
				auto* const dvar = game::Dvar_FindVar(con->auto_complete_matches[0].c_str());
				char description_buffer[256]{};
				std::string current_value{};
				std::string default_value{};
				std::string domain{};
				if (dvar)
				{
					const char* current_value_ptr = nullptr;
					const char* default_value_ptr = nullptr;
					if (try_get_dvar_value_ptr(dvar, dvar->current, &current_value_ptr))
					{
						std::string raw_value{};
						if (try_copy_c_string(current_value_ptr, raw_value))
						{
							current_value = sanitize_display_text(raw_value);
						}
					}

					if (try_get_dvar_value_ptr(dvar, dvar->reset, &default_value_ptr))
					{
						std::string raw_value{};
						if (try_copy_c_string(default_value_ptr, raw_value))
						{
							default_value = sanitize_display_text(raw_value);
						}
					}

					game::dvar_type domain_type{};
					game::DvarLimits domain_limits{};
					if (try_copy_dvar_domain(dvar, domain_type, domain_limits))
					{
						domain = sanitize_display_text(dvars::dvar_get_domain(domain_type, domain_limits));
					}
				}
				const auto has_description = dvar
					&& safe_read_dvar_description(dvar, description_buffer, sizeof(description_buffer))
					&& !sanitize_display_text(description_buffer).empty();
				const auto has_domain = !domain.empty();
				const auto line_count = dvar ? 2 : 1;
				draw_hint_box(bounds, hint_x, line_count, color_hint_box);
				draw_hint_text(bounds, hint_x, 0, con->auto_complete_matches[0].c_str(), dvar ? color_dvar_match : color_cmd_match);

				if (dvar)
				{
					const auto offset = std::max(96.0f, (bounds.screen_max[0] - hint_x) / 2.6f);
					draw_hint_text(bounds, hint_x, 0, current_value.empty() ? "<unavailable>" : current_value.c_str(), color_dvar_value, offset);
					draw_hint_text(bounds, hint_x, 1, "  default", color_dvar_inactive);
					draw_hint_text(bounds, hint_x, 1, default_value.empty() ? "<unavailable>" : default_value.c_str(), color_dvar_inactive, offset);

					if (has_description || has_domain)
					{
						const auto details_offset_y = (line_count * bounds.font_height) + 16.0f;
						const auto detail_lines = (has_description ? 1 : 0) + (has_domain ? 1 : 0);
						draw_hint_box(bounds, hint_x, detail_lines, color_hint_box, details_offset_y);

						auto detail_line = 0;
						if (has_description)
						{
							const auto description = sanitize_display_text(description_buffer);
							draw_hint_text(bounds, hint_x, detail_line, description.c_str(), color_dvar_inactive, 0.0f, details_offset_y);
							++detail_line;
						}

						if (has_domain)
						{
							draw_hint_text(bounds, hint_x, detail_line, domain.c_str(), color_dvar_inactive, 0.0f, details_offset_y);
						}
					}
				}

				return;
			}

			draw_hint_box(bounds, hint_x, static_cast<int>(con->auto_complete_matches.size()), color_hint_box);
			const auto offset = std::max(96.0f, (bounds.screen_max[0] - hint_x) / 2.6f);
			for (std::size_t i = 0; i < con->auto_complete_matches.size(); ++i)
			{
				auto* const dvar = game::Dvar_FindVar(con->auto_complete_matches[i].c_str());
				draw_hint_text(bounds, hint_x, static_cast<int>(i), con->auto_complete_matches[i].c_str(), dvar ? color_dvar_match : color_cmd_match);
				if (dvar)
				{
					std::string current_value{};
					const char* current_value_ptr = nullptr;
					if (try_get_dvar_value_ptr(dvar, dvar->current, &current_value_ptr))
					{
						std::string raw_value{};
						if (try_copy_c_string(current_value_ptr, raw_value))
						{
							current_value = sanitize_display_text(raw_value);
						}
					}
					draw_hint_text(bounds, hint_x, static_cast<int>(i), current_value.empty() ? "<unavailable>" : current_value.c_str(), color_dvar_value, offset);
				}
			}
		}

		void draw_output_scrollbar(const float x, float y, const float width, const float height, const int visible_lines, const int total_lines)
		{
			if (!con)
			{
				return;
			}

			const auto bar_x = (x + width) - 10.0f;
			draw_box(bar_x, y, 10.0f, height, color_output_bar);

			auto slider_height = height;
			if (total_lines > std::max(1, visible_lines))
			{
				const auto visible = static_cast<float>(std::max(1, visible_lines));
				const auto total = static_cast<float>(total_lines);
				const auto percentage = visible / total;
				slider_height *= percentage;

				const auto remaining_space = height - slider_height;
				const auto max_scroll = std::max(1, total_lines - visible_lines);
				const auto percentage_above = static_cast<float>(con->scroll_offset) / static_cast<float>(max_scroll);
				y += remaining_space * percentage_above;
			}

			draw_box(bar_x, y, 10.0f, slider_height, color_output_slider);
		}

		void draw_output_window(const overlay_bounds& bounds)
		{
			if (!con)
			{
				return;
			}

			const float output_y = con->output_fullscreen
				? bounds.y + bounds.font_height + 10.0f
				: bounds.screen_min[1] + 32.0f;
			const bool match_view = !con->auto_complete_query.empty() && !con->auto_complete_matches.empty();
			const auto height_scale = con->output_fullscreen
				? 1.0f
				: (con_outputHeightScale ? std::clamp(con_outputHeightScale->current.value, 0.20f, 0.98f) : 0.72f);
			const float remaining_height = std::max(48.0f, bounds.screen_max[1] - output_y);
			const float footer_height = bounds.font_height + 10.0f;
			float output_height = con->output_fullscreen
				? remaining_height
				: remaining_height * height_scale;

			if (con->output_fullscreen && match_view)
			{
				const auto desired_lines = std::max(1, static_cast<int>(con->auto_complete_matches.size()));
				const float desired_content_height = (desired_lines * bounds.font_height) + 6.0f;
				const float desired_output_height = desired_content_height + footer_height + 12.0f;
				output_height = std::min(remaining_height, desired_output_height);
			}
			draw_box(bounds.screen_min[0], output_y,
				bounds.screen_max[0] - bounds.screen_min[0],
				output_height,
				color_output_window);

			const float x = bounds.screen_min[0] + 6.0f;
			const float y = output_y + 6.0f;
			const float width = (bounds.screen_max[0] - bounds.screen_min[0]) - 12.0f;
			const float height = output_height - 12.0f;
			const float content_height = std::max(24.0f, height - footer_height);
			const auto visible_lines = std::max(1, static_cast<int>(content_height / std::max(1.0f, bounds.font_height)));

			if (match_view)
			{
				const auto max_scroll = std::max(0, static_cast<int>(con->auto_complete_matches.size()) - visible_lines);
				con->scroll_offset = std::clamp(con->scroll_offset, 0, max_scroll);
				const auto first_line = std::clamp(con->scroll_offset, 0, max_scroll);

				for (int i = 0; i < visible_lines; ++i)
				{
					const auto index = i + first_line;
					if (index >= static_cast<int>(con->auto_complete_matches.size()))
					{
						break;
					}

					const auto line_y = y + bounds.font_height + (bounds.font_height * i);
					draw_text(con->auto_complete_matches[static_cast<std::size_t>(index)].c_str(), x, line_y, color_white, 1.0f);
				}

				draw_output_scrollbar(x, y, width, content_height, visible_lines, static_cast<int>(con->auto_complete_matches.size()));
			}
			else
			{
				const auto max_scroll = std::max(0, static_cast<int>(con->lines.size()) - visible_lines);
				con->scroll_offset = std::clamp(con->scroll_offset, 0, max_scroll);
				const auto first_line = std::max(0, static_cast<int>(con->lines.size()) - visible_lines - con->scroll_offset);
				const auto offset = con->lines.size() >= static_cast<std::size_t>(visible_lines)
					? 0.0f
					: (bounds.font_height * (visible_lines - static_cast<int>(con->lines.size())));

				for (int i = 0; i < visible_lines; ++i)
				{
					const auto index = i + first_line;
					if (index >= static_cast<int>(con->lines.size()))
					{
						break;
					}

					const auto line_y = y + bounds.font_height + (bounds.font_height * i) + offset;
					draw_text(con->lines[static_cast<std::size_t>(index)].c_str(), x, line_y, color_white, 1.0f);
				}

				draw_output_scrollbar(x, y, width, content_height, visible_lines, static_cast<int>(con->lines.size()));
			}

			const auto version_text = build_full_version_string();
			draw_text_shadowed(version_text.c_str(), x, y + content_height + bounds.font_height + 5.0f, color_version_footer, 1.0f);
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

			const auto bounds = get_overlay_bounds();
			if (bounds.screen_max[0] <= bounds.screen_min[0] || bounds.screen_max[1] <= bounds.screen_min[1])
			{
				return;
			}
			if (con->output_visible)
			{
				draw_output_window(bounds);
			}
			draw_input(bounds);
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
			(void)time;

			if (overlay_active && down)
			{
				switch (key)
				{
				case game::K_MWHEELUP:
					scroll_output(3);
					break;
				case game::K_MWHEELDOWN:
					scroll_output(-3);
					break;
				default:
					break;
				}
			}

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
			set_cursor_position(0);
			con->history_index = -1;
			con->scroll_offset = 0;
			clear_auto_complete();
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
			set_cursor_position(con->input.size());
			refresh_auto_complete();
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
				set_cursor_position(0);
				return;
			}

			con->input = con->history[static_cast<std::size_t>(con->history_index)];
			set_cursor_position(con->input.size());
			refresh_auto_complete();
		}

		void insert_character(const char ch)
		{
			if (!con)
			{
				return;
			}

			delete_input_selection();
			if (con->input.size() >= max_input_chars)
			{
				return;
			}

			con->input.insert(con->input.begin() + static_cast<std::ptrdiff_t>(con->cursor), ch);
			++con->cursor;
			clear_input_selection();
			refresh_auto_complete();
		}

		void insert_text(std::string text)
		{
			if (!con || text.empty())
			{
				return;
			}

			text.erase(std::remove_if(text.begin(), text.end(), [](const unsigned char c)
				{
					return c == '\r' || c == '\n' || c == '\t';
				}), text.end());

			if (text.empty())
			{
				return;
			}

			const auto selection_length = has_input_selection() ? (get_selection_end() - get_selection_begin()) : 0;
			const auto current_length = con->input.size() - selection_length;
			const auto remaining = max_input_chars - current_length;
			if (remaining == 0)
			{
				return;
			}

			if (text.size() > remaining)
			{
				text.resize(remaining);
			}

			delete_input_selection();
			con->input.insert(con->cursor, text);
			con->cursor += text.size();
			clear_input_selection();
			refresh_auto_complete();
		}

		void paste_from_clipboard()
		{
			if (!OpenClipboard(nullptr))
			{
				return;
			}

			const auto clipboard_handle = GetClipboardData(CF_TEXT);
			if (!clipboard_handle)
			{
				CloseClipboard();
				return;
			}

			const auto* const clipboard_text = static_cast<const char*>(GlobalLock(clipboard_handle));
			if (!clipboard_text)
			{
				CloseClipboard();
				return;
			}

			insert_text(clipboard_text);
			GlobalUnlock(clipboard_handle);
			CloseClipboard();
		}

		bool handle_ctrl_shortcut(const int vk)
		{
			if (!is_any_ctrl_down())
			{
				return false;
			}

			switch (vk)
			{
			case 'A':
				select_entire_input();
				refresh_auto_complete();
				return true;
			case 'C':
				if (!is_any_shift_down())
				{
					return false;
				}

				if (con && !con->input.empty())
				{
					if (has_input_selection())
					{
						copy_text_to_clipboard(std::string_view(con->input).substr(get_selection_begin(), get_selection_end() - get_selection_begin()));
					}
					else
					{
						copy_text_to_clipboard(con->input);
					}
				}
				return true;
			case 'V':
				paste_from_clipboard();
				return true;
			case 'L':
				if (con)
				{
					con->lines.clear();
					con->scroll_offset = 0;
				}
				return true;
			default:
				return false;
			}
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
				if (delete_input_selection())
				{
					refresh_auto_complete();
				}
				else if (con && is_any_ctrl_down())
				{
					clear_input_line();
				}
				else if (con && con->cursor > 0)
				{
					con->input.erase(con->input.begin() + static_cast<std::ptrdiff_t>(con->cursor - 1));
					--con->cursor;
					refresh_auto_complete();
				}
				break;
			case VK_DELETE:
				if (delete_input_selection())
				{
					refresh_auto_complete();
				}
				else if (con && con->cursor < con->input.size())
				{
					con->input.erase(con->input.begin() + static_cast<std::ptrdiff_t>(con->cursor));
					refresh_auto_complete();
				}
				break;
			case VK_LEFT:
				if (con && con->cursor > 0)
				{
					set_cursor_position(con->cursor - 1);
					refresh_auto_complete();
				}
				break;
			case VK_RIGHT:
				if (con && con->cursor < con->input.size())
				{
					set_cursor_position(con->cursor + 1);
					refresh_auto_complete();
				}
				break;
			case VK_HOME:
				if (con)
				{
					set_cursor_position(0);
					refresh_auto_complete();
				}
				break;
			case VK_END:
				if (con)
				{
					set_cursor_position(con->input.size());
					refresh_auto_complete();
				}
				break;
			case VK_UP:
				if (con && con->output_visible && is_any_shift_down())
				{
					scroll_output(1);
				}
				else
				{
					history_up();
				}
				break;
			case VK_DOWN:
				if (con && con->output_visible && is_any_shift_down())
				{
					scroll_output(-1);
				}
				else
				{
					history_down();
				}
				break;
			case VK_PRIOR:
				scroll_output(6);
				break;
			case VK_NEXT:
				scroll_output(-6);
				break;
			case VK_TAB:
				if (con && is_any_shift_down())
				{
					if (is_any_ctrl_down())
					{
						con->output_visible = true;
						con->output_fullscreen = !con->output_fullscreen;
					}
					else
					{
						con->output_visible = !con->output_visible;
						if (!con->output_visible)
						{
							con->output_fullscreen = false;
						}
					}
				}
				else
				{
					handle_auto_complete();
				}
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
			if (result == 0)
			{
				return;
			}

			if (result < 0)
			{
				insert_wide_text(translated, 4);
				clear_dead_key_state();
				return;
			}

			insert_wide_text(translated, result);
		}

		void process_console_input()
		{
			if (!overlay_active || !is_game_focused() || !con)
			{
				return;
			}

			for (int vk = 8; vk < 256; ++vk)
			{
				key_is_down[static_cast<std::size_t>(vk)] = is_key_down(vk);
			}

			for (int vk = 8; vk < 256; ++vk)
			{
				const auto down = key_is_down[static_cast<std::size_t>(vk)];
				const auto key_index = static_cast<std::size_t>(vk);
				const auto now = GetTickCount();
				const auto first_press = down && !key_was_down[key_index];
				const bool repeatable = vk == VK_BACK || vk == VK_DELETE || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
				const auto repeat_press = down && key_was_down[key_index] && repeatable && now >= key_next_repeat_time[key_index];

				if (first_press || repeat_press)
				{
					if (handle_ctrl_shortcut(vk))
					{
						key_was_down[key_index] = down;
						key_next_repeat_time[key_index] = now + (first_press ? 350u : 45u);
						continue;
					}

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
					case VK_PRIOR:
					case VK_NEXT:
					case VK_TAB:
						handle_special_key(vk);
						break;
					default:
						handle_text_key(vk);
						break;
					}

					if (repeatable)
					{
						key_next_repeat_time[key_index] = now + (first_press ? 350u : 45u);
					}
				}
				else if (!down)
				{
					key_next_repeat_time[key_index] = 0;
				}

				key_was_down[key_index] = down;
			}
		}

		void safe_process_console_input()
		{
			if (process_shutting_down || !component_ready || !overlay_active || !con)
			{
				return;
			}

			const auto hwnd = get_window();
			if (!hwnd || !IsWindow(hwnd) || GetForegroundWindow() != hwnd)
			{
				return;
			}

			__try
			{
				process_console_input();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}
	}

	bool is_active()
	{
		return overlay_active;
	}

	void append_output(std::string_view text)
	{
		if (text.empty() || process_shutting_down || !component_ready)
		{
			return;
		}

		auto& pending_output = get_pending_output_queue();
		std::lock_guard _(get_pending_output_mutex());
		pending_output.emplace_back(text);

		while (pending_output.size() > max_console_lines * 2)
		{
			pending_output.pop_front();
		}
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

			scheduler::on_shutdown([]
				{
					component_ready = false;
					hooks_installed = false;
					set_overlay_active(false);
					key_was_down.fill(false);
					key_next_repeat_time.fill(0);
					con_outputHeightScale = nullptr;
				});

			scheduler::once([]
				{
					con_outputHeightScale = dvars::Dvar_RegisterFloat(
						"con_outputHeightScale",
						"Height scale for the shift-tab console output pane.",
						0.72f, 0.20f, 0.98f, game::dvar_flags::saved);
				}, scheduler::main);

			scheduler::loop([]
				{
					install_hooks_if_ready();

					if (!is_game_focused())
					{
						was_f1_down = false;
						was_oem5_down = false;
						was_oem102_down = false;
						key_was_down.fill(false);
						key_next_repeat_time.fill(0);
						flush_pending_output();
						return;
					}

					if (!component_ready)
					{
						return;
					}

					flush_pending_output();

					const auto f1_down = is_key_down(VK_F1);
					const auto oem5_down = is_key_down(VK_OEM_5);
					const auto oem102_down = is_key_down(VK_OEM_102);

					if (f1_down && !was_f1_down)
					{
						handle_toggle_press("F1");
					}

					if (oem5_down && !was_oem5_down)
					{
						if (is_any_shift_down())
						{
							handle_full_console_toggle_press("OEM_5");
						}
						else
						{
							handle_toggle_press("OEM_5");
						}
					}

					if (oem102_down && !was_oem102_down)
					{
						if (is_any_shift_down())
						{
							handle_full_console_toggle_press("OEM_102");
						}
						else
						{
							handle_toggle_press("OEM_102");
						}
					}

					was_f1_down = f1_down;
					was_oem5_down = oem5_down;
					was_oem102_down = oem102_down;
				}, scheduler::main, 50ms);

			// Process direct keyboard input through a guarded wrapper so transient
			// startup/input-state faults do not kill the whole game.
			scheduler::loop(safe_process_console_input, scheduler::main, 16ms);
		}

		void pre_destroy() override
		{
			process_shutting_down = true;
			component_ready = false;
			hooks_installed = false;
			set_overlay_active(false);
			cl_key_event_hook.clear();
			cl_console_print_hook.clear();
			con_set_console_rect_hook.clear();
			con = nullptr;
		}
	};
}

REGISTER_COMPONENT(game_console::component)
