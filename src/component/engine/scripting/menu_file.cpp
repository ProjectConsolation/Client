#ifndef CONSOLATION_MENU_PARSER_TEST
#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "filesystem.hpp"
#include "menu_file.hpp"

#include "game/game.hpp"

#include <utils/memory.hpp>
#include <utils/hook.hpp>
#include <utils/string.hpp>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace menu_file
{
	namespace
	{
		// ---------------------------------------------------------------
		// Tokenizer
		// ---------------------------------------------------------------
		// Handles the pieces of the grammar this parser cares about: // and
		// /* */ comments, quoted strings, bare tokens, and { } as standalone
		// tokens. Preprocessor directives are rejected until preprocessing
		// is implemented; silently dropping includes produces incomplete menus.
		class tokenizer
		{
		public:
			explicit tokenizer(const std::string& source) : source_(source)
			{
			}

			std::string next()
			{
				skip_ignorable();
				if (cursor_ >= source_.size())
				{
					return {};
				}

				const char ch = source_[cursor_];
				if (ch == '{' || ch == '}')
				{
					++cursor_;
					return std::string(1, ch);
				}

				if (ch == '"')
				{
					return read_quoted();
				}

				const auto start = cursor_;
				while (cursor_ < source_.size())
				{
					const char c = source_[cursor_];
					if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"')
					{
						break;
					}

					if (c == '/' && cursor_ + 1 < source_.size()
						&& (source_[cursor_ + 1] == '/' || source_[cursor_ + 1] == '*'))
					{
						break;
					}

					++cursor_;
				}

				return source_.substr(start, cursor_ - start);
			}

			std::string peek()
			{
				const auto saved_cursor = cursor_;
				const auto saved_line = line_;
				auto token = next();
				cursor_ = saved_cursor;
				line_ = saved_line;
				return token;
			}

		// Reads the raw text between a matching pair of braces, without
			// tokenizing its contents. Action scripts are passed to QoS's native
			// script runner; braces inside comments and strings are not delimiters.
			std::string read_balanced_block()
			{
				skip_ignorable();
				if (cursor_ >= source_.size() || source_[cursor_] != '{')
				{
					fail("expected '{'");
				}

				++cursor_;
				const auto start = cursor_;
				auto depth = 1;
				while (cursor_ < source_.size() && depth > 0)
				{
					const char c = source_[cursor_];
					if (c == '"')
					{
						read_quoted();
						continue;
					}
					if (source_.compare(cursor_, 2, "//") == 0 || source_.compare(cursor_, 2, "/*") == 0)
					{
						skip_ignorable();
						continue;
					}
					if (c == '{')
					{
						++depth;
					}
					else if (c == '}')
					{
						--depth;
					}

					if (c == '\n')
					{
						++line_;
					}

					++cursor_;
				}

				if (depth != 0)
				{
					fail("unterminated block");
				}

				return source_.substr(start, (cursor_ - 1) - start);
			}

			float number()
			{
				const auto value = next();
				char* end = nullptr;
				const float result = std::strtof(value.c_str(), &end);
				if (value.empty() || end != value.c_str() + value.size() || !std::isfinite(result))
				{
					fail(std::format("expected a number, got '{}'", value));
				}

				return result;
			}

			int integer()
			{
				const double value = number();
				if (std::trunc(value) != value || value < (std::numeric_limits<int>::min)()
					|| value > (std::numeric_limits<int>::max)()) fail("integer out of range");
				return static_cast<int>(value);
			}

			bool visibility()
			{
				if (peek() == "{") fail("visibility expressions are not implemented; use visible 0 or 1");
				const auto value = integer();
				if (value != 0 && value != 1) fail("visible must be 0 or 1");
				return value != 0;
			}

			// True if the next token parses cleanly as a float, without
			// consuming it. Used for optional trailing numeric fields
			// (e.g. rect's optional horzAlign/vertAlign).
			bool next_is_number()
			{
				const auto value = peek();
				if (value.empty())
				{
					return false;
				}

				char* end = nullptr;
				std::strtof(value.c_str(), &end);
				return end == value.c_str() + value.size();
			}

			bool at_end()
			{
				skip_ignorable();
				return cursor_ >= source_.size();
			}

			int line() const
			{
				return line_;
			}

			[[noreturn]] void fail(const std::string& message) const
			{
				throw std::runtime_error(std::format("line {}: {}", line_, message));
			}

		private:
			const std::string& source_;
			std::size_t cursor_ = 0;
			int line_ = 1;

			void skip_ignorable()
			{
				for (;;)
				{
					while (cursor_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[cursor_])))
					{
						if (source_[cursor_] == '\n')
						{
							++line_;
						}

						++cursor_;
					}

					if (cursor_ + 1 < source_.size() && source_[cursor_] == '/' && source_[cursor_ + 1] == '/')
					{
						const auto end = source_.find('\n', cursor_);
						cursor_ = end == std::string::npos ? source_.size() : end;
						continue;
					}

					if (cursor_ + 1 < source_.size() && source_[cursor_] == '/' && source_[cursor_ + 1] == '*')
					{
						const auto end = source_.find("*/", cursor_ + 2);
						if (end == std::string::npos) fail("unterminated comment");
						const auto stop = end == std::string::npos ? source_.size() : end + 2;
						for (auto i = cursor_; i < stop; ++i)
						{
							if (source_[i] == '\n')
							{
								++line_;
							}
						}

						cursor_ = stop;
						continue;
					}

					if (cursor_ < source_.size() && source_[cursor_] == '#')
					{
						fail("#include/#define preprocessing is not implemented");
					}

					break;
				}
			}

			std::string read_quoted()
			{
				++cursor_; // opening quote
				std::string value;
				while (cursor_ < source_.size())
				{
					const char c = source_[cursor_++];
					if (c == '"')
					{
						return value;
					}

					if (c == '\n')
					{
						fail("unterminated string");
					}

					if (c == '\\' && cursor_ < source_.size())
					{
						const char escaped = source_[cursor_++];
						value.push_back(escaped == 'n' ? '\n' : escaped);
						continue;
					}

					value.push_back(c);
				}

				fail("unterminated string");
			}
		};

		// ---------------------------------------------------------------
		// Intermediate representation
		// ---------------------------------------------------------------
		struct parsed_rect
		{
			float x = 0, y = 0, w = 0, h = 0;
			int horz_align = 0;
			int vert_align = 0;
		};

		struct parsed_item
		{
			std::string name;
			parsed_rect rect{};
			int type = 0; // ITEM_TYPE_*, see item_type_keywords
			int style = 0;
			int border = 0;
			float border_size = 0;
			bool decoration = false;
			bool visible = true;
			std::array<float, 4> fore_color{1, 1, 1, 1};
			std::array<float, 4> back_color{0, 0, 0, 0};
			std::array<float, 4> border_color{0, 0, 0, 0};
			std::string text;
			int font_enum = 0; // best-effort, see resolve_font_enum()
			float text_scale = 1.0f;
			int text_style = 0;
			int text_align_mode = 0;
			float text_align_x = 0, text_align_y = 0;
			std::string action, on_focus, on_accept, leave_focus;
			std::string mouse_enter, mouse_exit, mouse_enter_text, mouse_exit_text;
			std::string dvar, dvar_test, enable_dvar;

			bool has_listbox = false;
			float lb_element_width = 0, lb_element_height = 0;
			int lb_element_style = 0;
			int lb_feeder = 0;

			bool has_editfield = false;
			float ef_min = 0, ef_max = 0, ef_def = 0, ef_range = 0;
			int ef_max_chars = 0, ef_max_chars_goto_next = 0, ef_max_paint_chars = 0;
		};

		struct parsed_menu
		{
			std::string name;
			parsed_rect rect{0, 0, 640, 480};
			int full_screen = 0;
			bool visible = false;
			int style = 0;
			std::array<float, 4> fore_color{1, 1, 1, 1};
			std::array<float, 4> back_color{0, 0, 0, 0};
			std::array<float, 4> border_color{0, 0, 0, 0};
			std::array<float, 4> outline_color{0, 0, 0, 0};
			std::array<float, 4> focus_color{1, 1, 1, 1};
			std::array<float, 4> disable_color{1, 1, 1, 1};
			std::string font = "fonts/normal";
			std::string on_open, on_close, on_esc, sound_name, allowed_binding;
			int fade_cycle = 0;
			float fade_clamp = 0, fade_in_amount = 0, blur_radius = 0;
			std::vector<parsed_item> items;
		};

		// ---------------------------------------------------------------
		// Field-value readers
		// ---------------------------------------------------------------
		parsed_rect read_rect(tokenizer& tok)
		{
			parsed_rect rect{};
			rect.x = tok.number();
			rect.y = tok.number();
			rect.w = tok.number();
			rect.h = tok.number();
			if (tok.next_is_number())
			{
				rect.horz_align = tok.integer();
			}

			if (tok.next_is_number())
			{
				rect.vert_align = tok.integer();
			}

			return rect;
		}

		std::array<float, 4> read_color4(tokenizer& tok)
		{
			return {tok.number(), tok.number(), tok.number(), tok.number()};
		}

		std::string lower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
			return value;
		}

		// Best-effort ITEM_TYPE_* keyword table. Names are confirmed against
		// this build's own runtime error strings ("Menu Error: Expecting
		// type: ITEM_TYPE_LISTBOX", "...ITEM_TYPE_EDITFIELD, ITEM_TYPE_..."
		// etc. found in jb_mp_s.dll), but the *numeric* assignment follows
		// the long-standing IW-engine convention and has not been verified
		// bit-for-bit against this specific binary. If an item renders or
		// behaves as the wrong control, check this table first.
		const std::unordered_map<std::string, int>& item_type_keywords()
		{
			static const std::unordered_map<std::string, int> table{
				{"item_type_text", 0},
				{"item_type_button", 1},
				{"item_type_radiobutton", 2},
				{"item_type_checkbox", 3},
				{"item_type_editfield", 4},
				{"item_type_combo", 5},
				{"item_type_listbox", 6},
				{"item_type_model", 7},
				{"item_type_ownerdraw", 8},
				{"item_type_numericfield", 9},
				{"item_type_slider", 10},
				{"item_type_yesno", 11},
				{"item_type_multi", 12},
				{"item_type_dvarenum", 13},
				{"item_type_bind", 14},
				{"item_type_menumodel", 15},
				{"item_type_validfilefield", 16},
				{"item_type_decimalfield", 17},
				{"item_type_upreditfield", 18},
			};
			return table;
		}

		// Same caveat as item_type_keywords(): names are the well-known
		// IW-engine convention, not yet cross-checked against this binary's
		// window-style renderer. Numeric `style N` is always accepted too.
		const std::unordered_map<std::string, int>& window_style_keywords()
		{
			static const std::unordered_map<std::string, int> table{
				{"window_style_empty", 0},
				{"window_style_filled", 1},
				{"window_style_gradient", 2},
				{"window_style_shader", 3},
				{"window_style_teamcolor", 4},
			};
			return table;
		}

		int read_type_keyword(tokenizer& tok, const std::unordered_map<std::string, int>& table, const char* field_name)
		{
			const auto token = tok.next();
			if (const auto it = table.find(lower(token)); it != table.end())
			{
				return it->second;
			}

			char* end = nullptr;
			const auto value = std::strtol(token.c_str(), &end, 10);
			if (!token.empty() && end == token.c_str() + token.size())
			{
				return static_cast<int>(value);
			}

			tok.fail(std::format("unrecognized {} value '{}'", field_name, token));
		}

		// Font is stored in itemDef_s as an int enum (fontEnum), not a
		// string, and this build's font-index table has not been located
		// yet. Anything other than "default" logs and falls back to 0 --
		// treat this as a placeholder until the real table is confirmed.
		int resolve_font_enum(tokenizer& tok, const std::string& name)
		{
			const auto normalized = lower(name);
			if (normalized == "default" || normalized.empty())
			{
				return 0;
			}

			console::warn("[menu - parse] line %d: unrecognized font '%s', using index 0 (needs verifying against this build)\n",
				tok.line(), name.c_str());
			return 0;
		}

		// Reject unsupported fields rather than consuming the next field as a
		// guessed value and publishing a partially initialized menu.
		void skip_unknown_field(tokenizer& tok, const std::string& field_name)
		{
			tok.fail(std::format("unsupported field '{}'", field_name));
		}

		// ---------------------------------------------------------------
		// itemDef parsing
		// ---------------------------------------------------------------
		using item_field_fn = std::function<void(tokenizer&, parsed_item&)>;

		const std::unordered_map<std::string, item_field_fn>& item_field_table()
		{
			static const std::unordered_map<std::string, item_field_fn> table{
				{"name", [](tokenizer& t, parsed_item& i) { i.name = t.next(); }},
				{"rect", [](tokenizer& t, parsed_item& i) { i.rect = read_rect(t); }},
				{"type", [](tokenizer& t, parsed_item& i) { i.type = read_type_keyword(t, item_type_keywords(), "type"); }},
				{"style", [](tokenizer& t, parsed_item& i) { i.style = read_type_keyword(t, window_style_keywords(), "style"); }},
				{"border", [](tokenizer& t, parsed_item& i) { i.border = t.integer(); }},
				{"bordersize", [](tokenizer& t, parsed_item& i) { i.border_size = t.number(); }},
				{"decoration", [](tokenizer&, parsed_item& i) { i.decoration = true; }},
				{"forecolor", [](tokenizer& t, parsed_item& i) { i.fore_color = read_color4(t); }},
				{"backcolor", [](tokenizer& t, parsed_item& i) { i.back_color = read_color4(t); }},
				{"bordercolor", [](tokenizer& t, parsed_item& i) { i.border_color = read_color4(t); }},
				{"text", [](tokenizer& t, parsed_item& i) { i.text = t.next(); }},
				{"font", [](tokenizer& t, parsed_item& i) { const auto name = t.next(); i.font_enum = resolve_font_enum(t, name); }},
				{"textscale", [](tokenizer& t, parsed_item& i) { i.text_scale = t.number(); }},
				{"textstyle", [](tokenizer& t, parsed_item& i) { i.text_style = t.integer(); }},
				{"textalign", [](tokenizer& t, parsed_item& i) { i.text_align_mode = t.integer(); }},
				{"textalignx", [](tokenizer& t, parsed_item& i) { i.text_align_x = t.number(); }},
				{"textaligny", [](tokenizer& t, parsed_item& i) { i.text_align_y = t.number(); }},
				{"action", [](tokenizer& t, parsed_item& i) { i.action = t.read_balanced_block(); }},
				{"onfocus", [](tokenizer& t, parsed_item& i) { i.on_focus = t.read_balanced_block(); }},
				{"onaccept", [](tokenizer& t, parsed_item& i) { i.on_accept = t.read_balanced_block(); }},
				{"leavefocus", [](tokenizer& t, parsed_item& i) { i.leave_focus = t.read_balanced_block(); }},
				{"mouseenter", [](tokenizer& t, parsed_item& i) { i.mouse_enter = t.read_balanced_block(); }},
				{"mouseexit", [](tokenizer& t, parsed_item& i) { i.mouse_exit = t.read_balanced_block(); }},
				{"mouseenter_text", [](tokenizer& t, parsed_item& i) { i.mouse_enter_text = t.read_balanced_block(); }},
				{"mouseexittext", [](tokenizer& t, parsed_item& i) { i.mouse_exit_text = t.read_balanced_block(); }},
				{"dvar", [](tokenizer& t, parsed_item& i) { i.dvar = t.next(); }},
				{"dvartest", [](tokenizer& t, parsed_item& i) { i.dvar_test = t.next(); }},
				{"enabledvar", [](tokenizer& t, parsed_item& i) { i.enable_dvar = t.next(); }},
				// Only constant visibility is supported; reject expressions.
				{"visible", [](tokenizer& t, parsed_item& i) { i.visible = t.visibility(); }},
				{"disabled", [](tokenizer& t, parsed_item&) { t.fail("disabled expressions are not implemented"); }},
				// itemDefData_t: listBoxDef_s
				{"elementwidth", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_element_width = t.number(); }},
				{"elementheight", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_element_height = t.number(); }},
				{"elementtype", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_element_style = t.integer(); }},
				{"feeder", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_feeder = t.integer(); }},
				// itemDefData_t: editFieldDef_s
				{"maxchars", [](tokenizer& t, parsed_item& i) { i.has_editfield = true; i.ef_max_chars = t.integer(); }},
				{"maxcharsgotonext", [](tokenizer& t, parsed_item& i) { i.has_editfield = true; i.ef_max_chars_goto_next = t.integer(); }},
				{"maxpaintchars", [](tokenizer& t, parsed_item& i) { i.has_editfield = true; i.ef_max_paint_chars = t.integer(); }},
				{"minval", [](tokenizer& t, parsed_item& i) { i.has_editfield = true; i.ef_min = t.number(); }},
				{"maxval", [](tokenizer& t, parsed_item& i) { i.has_editfield = true; i.ef_max = t.number(); }},
				{"defval", [](tokenizer& t, parsed_item& i) { i.has_editfield = true; i.ef_def = t.number(); }},
			};
			return table;
		}

		parsed_item parse_item(tokenizer& tok)
		{
			parsed_item item{};
			if (tok.next() != "{")
			{
				tok.fail("expected '{' after itemDef");
			}

			for (;;)
			{
				auto field = tok.next();
				if (field.empty())
				{
					tok.fail("unexpected end of file inside itemDef");
				}

				if (field == "}")
				{
					break;
				}

				const auto& table = item_field_table();
				if (const auto it = table.find(lower(field)); it != table.end())
				{
					it->second(tok, item);
				}
				else
				{
					skip_unknown_field(tok, field);
				}
			}

			if (item.name.empty())
			{
				tok.fail("itemDef is missing a 'name' field");
			}

			return item;
		}

		// ---------------------------------------------------------------
		// menuDef parsing
		// ---------------------------------------------------------------
		using menu_field_fn = std::function<void(tokenizer&, parsed_menu&)>;

		const std::unordered_map<std::string, menu_field_fn>& menu_field_table()
		{
			static const std::unordered_map<std::string, menu_field_fn> table{
				{"name", [](tokenizer& t, parsed_menu& m) { m.name = t.next(); }},
				{"rect", [](tokenizer& t, parsed_menu& m) { m.rect = read_rect(t); }},
				{"fullscreen", [](tokenizer& t, parsed_menu& m) { m.full_screen = t.integer(); }},
				{"style", [](tokenizer& t, parsed_menu& m) { m.style = read_type_keyword(t, window_style_keywords(), "style"); }},
				{"forecolor", [](tokenizer& t, parsed_menu& m) { m.fore_color = read_color4(t); }},
				{"backcolor", [](tokenizer& t, parsed_menu& m) { m.back_color = read_color4(t); }},
				{"bordercolor", [](tokenizer& t, parsed_menu& m) { m.border_color = read_color4(t); }},
				{"outlinecolor", [](tokenizer& t, parsed_menu& m) { m.outline_color = read_color4(t); }},
				{"focuscolor", [](tokenizer& t, parsed_menu& m) { m.focus_color = read_color4(t); }},
				{"disablecolor", [](tokenizer& t, parsed_menu& m) { m.disable_color = read_color4(t); }},
				{"font", [](tokenizer& t, parsed_menu& m) { m.font = t.next(); }},
				{"onopen", [](tokenizer& t, parsed_menu& m) { m.on_open = t.read_balanced_block(); }},
				{"onclose", [](tokenizer& t, parsed_menu& m) { m.on_close = t.read_balanced_block(); }},
				{"onesc", [](tokenizer& t, parsed_menu& m) { m.on_esc = t.read_balanced_block(); }},
				{"soundname", [](tokenizer& t, parsed_menu& m) { m.sound_name = t.next(); }},
				{"allowedbinding", [](tokenizer& t, parsed_menu& m) { m.allowed_binding = t.next(); }},
				{"fadecycle", [](tokenizer& t, parsed_menu& m) { m.fade_cycle = t.integer(); }},
				{"fadeclamp", [](tokenizer& t, parsed_menu& m) { m.fade_clamp = t.number(); }},
				{"fadein", [](tokenizer& t, parsed_menu& m) { m.fade_in_amount = t.number(); }},
				{"blurworld", [](tokenizer& t, parsed_menu& m) { m.blur_radius = t.number(); }},
				// Expression fields not yet compiled -- see build_native_menu().
				{"visible", [](tokenizer& t, parsed_menu& m) { m.visible = t.visibility(); }},
			};
			return table;
		}

		parsed_menu parse_menu(tokenizer& tok)
		{
			parsed_menu menu{};
			if (tok.next() != "{")
			{
				tok.fail("expected '{' after menuDef");
			}

			for (;;)
			{
				auto field = tok.next();
				if (field.empty())
				{
					tok.fail("unexpected end of file inside menuDef");
				}

				if (field == "}")
				{
					break;
				}

				if (lower(field) == "itemdef")
				{
					if (menu.items.size() >= 512) tok.fail("too many menu items");
					menu.items.push_back(parse_item(tok));
					continue;
				}

				const auto& table = menu_field_table();
				if (const auto it = table.find(lower(field)); it != table.end())
				{
					it->second(tok, menu);
				}
				else
				{
					skip_unknown_field(tok, field);
				}
			}

			if (menu.name.empty())
			{
				tok.fail("menuDef is missing a 'name' field");
			}

			return menu;
		}

		std::vector<parsed_menu> parse_menu_file(const std::string& source)
		{
			tokenizer tok(source);
			std::vector<parsed_menu> menus;
			while (!tok.at_end())
			{
				const auto field = tok.next();
				if (field.empty())
				{
					break;
				}

				if (lower(field) != "menudef")
				{
					tok.fail(std::format("expected 'menuDef', got '{}'", field));
				}

				menus.push_back(parse_menu(tok));
			}

			return menus;
		}

#ifndef CONSOLATION_MENU_PARSER_TEST
		// ---------------------------------------------------------------
		// Native struct construction
		//
		// Expressions remain unsupported and are rejected by the parser.
		// Constant visibility uses the native visible flag (bit 4); expression
		// storage remains zeroed. Opening a menu still uses native UI logic.
		// ---------------------------------------------------------------
		const char* allocate_menu_string(const std::string& value)
		{
			auto* result = static_cast<char*>(utils::memory::allocate(value.size() + 1));
			std::memcpy(result, value.data(), value.size());
			result[value.size()] = '\0';
			return result;
		}

		const char* allocate_menu_string_opt(const std::string& value)
		{
			return value.empty() ? nullptr : allocate_menu_string(value);
		}

		game::itemDef_s* build_native_item(const parsed_item& parsed, game::menuDef_t* parent)
		{
			auto* item = utils::memory::allocate<game::itemDef_s>();
			std::memset(item, 0, sizeof(*item));

			item->parent = parent;
			item->window.name = allocate_menu_string(parsed.name);
			item->window.rect = {parsed.rect.x, parsed.rect.y, parsed.rect.w, parsed.rect.h,
				parsed.rect.horz_align, parsed.rect.vert_align};
			item->window.rectClient = item->window.rect;
			item->window.style = parsed.style;
			item->window.border = parsed.border;
			item->window.borderSize = parsed.border_size;
			item->window.staticFlags = parsed.decoration ? 1 : 0;
			std::fill(std::begin(item->window.dynamicFlags), std::end(item->window.dynamicFlags), parsed.visible ? 4 : 0);
			std::copy(parsed.fore_color.begin(), parsed.fore_color.end(), item->window.foreColor);
			std::copy(parsed.back_color.begin(), parsed.back_color.end(), item->window.backColor);
			std::copy(parsed.border_color.begin(), parsed.border_color.end(), item->window.borderColor);

			item->type = parsed.type;
			item->fontEnum = parsed.font_enum;
			item->textscale = parsed.text_scale;
			item->textStyle = parsed.text_style;
			item->textAlignMode = parsed.text_align_mode;
			item->textalignx = parsed.text_align_x;
			item->textaligny = parsed.text_align_y != 0 ? parsed.text_align_y : parsed.rect.h * 0.75f;
			item->text = allocate_menu_string_opt(parsed.text);

			item->action = allocate_menu_string_opt(parsed.action);
			item->onFocus = allocate_menu_string_opt(parsed.on_focus);
			item->onAccept = allocate_menu_string_opt(parsed.on_accept);
			item->leaveFocus = allocate_menu_string_opt(parsed.leave_focus);
			item->mouseEnter = allocate_menu_string_opt(parsed.mouse_enter);
			item->mouseExit = allocate_menu_string_opt(parsed.mouse_exit);
			item->mouseEnterText = allocate_menu_string_opt(parsed.mouse_enter_text);
			item->mouseExitText = allocate_menu_string_opt(parsed.mouse_exit_text);
			item->dvar = allocate_menu_string_opt(parsed.dvar);
			item->dvarTest = allocate_menu_string_opt(parsed.dvar_test);
			item->enableDvar = allocate_menu_string_opt(parsed.enable_dvar);
			item->feeder = parsed.lb_feeder;

			if (parsed.has_listbox)
			{
				auto* listbox = utils::memory::allocate<game::listBoxDef_s>();
				std::memset(listbox, 0, sizeof(*listbox));
				listbox->elementWidth = parsed.lb_element_width;
				listbox->elementHeight = parsed.lb_element_height;
				listbox->elementStyle = parsed.lb_element_style;
				item->typeData.listBox = listbox;
			}
			else if (parsed.has_editfield)
			{
				auto* edit = utils::memory::allocate<game::editFieldDef_s>();
				std::memset(edit, 0, sizeof(*edit));
				edit->minVal = parsed.ef_min;
				edit->maxVal = parsed.ef_max;
				edit->defVal = parsed.ef_def;
				edit->range = parsed.ef_range;
				edit->maxChars = parsed.ef_max_chars;
				edit->maxCharsGotoNext = parsed.ef_max_chars_goto_next;
				edit->maxPaintChars = parsed.ef_max_paint_chars;
				item->typeData.editField = edit;
			}

			return item;
		}

		game::menuDef_t* build_native_menu(const parsed_menu& parsed)
		{
			auto* menu = utils::memory::allocate<game::menuDef_t>();
			std::memset(menu, 0, sizeof(*menu));

			menu->window.name = allocate_menu_string(parsed.name);
			menu->window.rect = {parsed.rect.x, parsed.rect.y, parsed.rect.w, parsed.rect.h,
				parsed.rect.horz_align, parsed.rect.vert_align};
			menu->window.rectClient = menu->window.rect;
			menu->window.style = parsed.style;
			std::fill(std::begin(menu->window.dynamicFlags), std::end(menu->window.dynamicFlags), parsed.visible ? 4 : 0);
			std::copy(parsed.fore_color.begin(), parsed.fore_color.end(), menu->window.foreColor);
			std::copy(parsed.back_color.begin(), parsed.back_color.end(), menu->window.backColor);
			std::copy(parsed.border_color.begin(), parsed.border_color.end(), menu->window.borderColor);
			std::copy(parsed.outline_color.begin(), parsed.outline_color.end(), menu->window.outlineColor);

			menu->font = allocate_menu_string(parsed.font);
			menu->fullScreen = parsed.full_screen;
			menu->fadeCycle = parsed.fade_cycle;
			menu->fadeClamp = parsed.fade_clamp;
			menu->fadeInAmount = parsed.fade_in_amount;
			menu->blurRadius = parsed.blur_radius;
			menu->onOpen = allocate_menu_string_opt(parsed.on_open);
			menu->onClose = allocate_menu_string_opt(parsed.on_close);
			menu->onESC = allocate_menu_string_opt(parsed.on_esc);
			menu->soundName = allocate_menu_string_opt(parsed.sound_name);
			menu->allowedBinding = allocate_menu_string_opt(parsed.allowed_binding);
			std::copy(parsed.focus_color.begin(), parsed.focus_color.end(), menu->focusColor);
			std::copy(parsed.disable_color.begin(), parsed.disable_color.end(), menu->disableColor);

			std::fill(std::begin(menu->cursorItem), std::end(menu->cursorItem), -1);

			menu->itemCount = static_cast<int>(parsed.items.size());
			menu->items = utils::memory::allocate_array<game::itemDef_s*>(std::max(menu->itemCount, 1));
			for (auto i = 0; i < menu->itemCount; ++i)
			{
				menu->items[i] = build_native_item(parsed.items[i], menu);
			}

			return menu;
		}

		// ---------------------------------------------------------------
		// Disk scanning / cache
		// ---------------------------------------------------------------
		std::unordered_map<std::string, game::menuDef_t*> loaded_menus;
		std::unordered_map<std::string, std::string> loaded_menu_sources;
		bool scanned = false;
		utils::hook::detour open_menu_hook;

		void open_menu_stub()
		{
			const command::params args;
			if (args.size() != 2)
			{
				console::info("[menu - open] usage: openmenu <menu name or .menu path>\n");
				open_menu_hook.invoke<void>();
				return;
			}
			auto name = std::string(args.get(1));
			console::info("[menu - open] opening menu '%s'...\n", name.c_str());
			std::replace(name.begin(), name.end(), '\\', '/');
			if (lower(name).ends_with(".menu")) name = std::filesystem::path(name).stem().string();
			auto* menu = find(name);
			if (!menu)
			{
				console::info("[menu - open] no disk definition for '%s'; trying native menu lookup\n", name.c_str());
				open_menu_hook.invoke<void>();
				return;
			}
			const auto source = loaded_menu_sources.find(lower(menu->window.name));
			console::info("[menu - open] resolved '%s' from disk '%s' (%d items, cached parsed definition)\n",
				menu->window.name, source != loaded_menu_sources.end() ? source->second.c_str() : "<source unavailable>", menu->itemCount);

			// QoS UI_OpenMenu_f (0x102E1560) passes this context to
			// 0x102D8B90. Its lookup reads count +2104 and pointers +56.
			auto* context = reinterpret_cast<unsigned char*>(game::game_offset(0x113CFC38));
			auto& count = *reinterpret_cast<int*>(context + 2104);
			auto** menus = reinterpret_cast<game::menuDef_t**>(context + 56);
			if (count < 0 || count > 512)
			{
				console::warn("[menu - disk] invalid UI menu count: %d\n", count);
				return;
			}
			int slot = 0;
			for (; slot < count; ++slot)
			{
				if (menus[slot] && menus[slot]->window.name && !_stricmp(menus[slot]->window.name, menu->window.name)) break;
			}
			if (slot == count)
			{
				if (count == 512)
				{
					console::warn("[menu - disk] UI menu capacity reached\n");
					return;
				}
				++count;
			}
			else if (menus[slot] != menu)
			{
				// Close the previous generation before replacing a reloaded menu.
				utils::hook::invoke<void>(game::game_offset(0x102CE070), context, menus[slot]);
			}
			menus[slot] = menu;
			const auto opened = utils::hook::invoke<int>(game::game_offset(0x102D8B90), context, menu->window.name);
			if (opened) *game::keyCatchers |= 0x10;
			const auto local_client = *reinterpret_cast<int*>(context);
			const auto flags = local_client >= 0 && local_client < 4 ? menu->window.dynamicFlags[local_client] : 0;
			console::info("[menu - disk] native open '%s': result=%d slot=%d flags=0x%X items=%d\n",
				menu->window.name, opened, slot, flags, menu->itemCount);
		}

		void parse_and_register(const std::filesystem::path& disk_path)
		{
			std::ifstream stream(disk_path, std::ios::binary | std::ios::ate);
			if (!stream)
			{
				return;
			}

			const auto length = static_cast<std::streamoff>(stream.tellg());
			if (length <= 0 || length > 1024 * 1024)
			{
				console::warn("[menu - disk] invalid file size: %s\n", disk_path.string().c_str());
				return;
			}
			const auto size = static_cast<std::size_t>(length);
			std::string source(size, '\0');
			stream.seekg(0, std::ios::beg);
			if (!stream.read(source.data(), static_cast<std::streamsize>(size)))
			{
				console::warn("[menu - disk] read failed: %s\n", disk_path.string().c_str());
				return;
			}

			std::vector<parsed_menu> menus;
			try
			{
				menus = parse_menu_file(source);
			}
			catch (const std::exception& error)
			{
				console::warn("[menu - disk] rejected %s: %s\n", disk_path.string().c_str(), error.what());
				return;
			}

			for (const auto& parsed : menus)
			{
				const auto key = lower(parsed.name);
				if (loaded_menus.contains(key))
				{
					console::warn("[menu - disk] duplicate menu name '%s' in %s, keeping first definition\n",
						parsed.name.c_str(), disk_path.string().c_str());
					continue;
				}

				auto* native = build_native_menu(parsed);
				loaded_menus.emplace(key, native);
				std::error_code path_error;
				const auto absolute_path = std::filesystem::absolute(disk_path, path_error);
				loaded_menu_sources.emplace(key, (path_error ? disk_path : absolute_path).lexically_normal().string());
				console::info("[menu - disk] loaded '%s' (%d items) from %s\n",
					parsed.name.c_str(), native->itemCount, disk_path.string().c_str());
			}
		}

		void scan_search_paths()
		{
			for (const auto& search_path : filesystem::get_search_paths())
			{
				std::error_code error{};
				const std::filesystem::path root(search_path);
				if (!std::filesystem::exists(root, error) || error)
				{
					continue;
				}

				std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error);
				const std::filesystem::recursive_directory_iterator end;
				for (; !error && it != end; it.increment(error))
				{
					const auto& entry = *it;
					if (!entry.is_regular_file())
					{
						continue;
					}

					auto extension = entry.path().extension().string();
					std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c)
					{
						return static_cast<char>(std::tolower(c));
					});

					if (extension == ".menu")
					{
						parse_and_register(entry.path());
					}
				}
			}
		}

		void register_commands()
		{
			command::add("reloadMenus", [](const command::params&)
			{
				if (reload()) console::info("reloadMenus: %zu custom menu(s) loaded; reopen the menu to test changes\n", loaded_menus.size());
			});
		}

		class component final : public component_interface
		{
		public:
			void post_load() override
			{
				open_menu_hook.create(game::game_offset(0x102E1560), open_menu_stub);
				console::info("[menu - open] installed: native openmenu hook with disk-source diagnostics\n");
				register_commands();
			}

			void pre_destroy() override
			{
				open_menu_hook.clear();
				loaded_menus.clear();
				loaded_menu_sources.clear();
				scanned = false;
			}
		};
#endif
	}

#ifndef CONSOLATION_MENU_PARSER_TEST
	void ensure_loaded()
	{
		if (scanned)
		{
			return;
		}

		scanned = true;
		scan_search_paths();
	}

	bool reload()
	{
		// QoS's registered array and open stack are separate. Never replace
		// a menu still referenced by the stack, even if it is currently hidden.
		auto* context = reinterpret_cast<unsigned char*>(game::game_offset(0x113CFC38));
		auto& count = *reinterpret_cast<int*>(context + 2104);
		const auto open_count = *reinterpret_cast<const int*>(context + 2172);
		auto** menus = reinterpret_cast<game::menuDef_t**>(context + 56);
		auto** open_menus = reinterpret_cast<game::menuDef_t**>(context + 2108);
		if (count < 0 || count > 512 || open_count < 0 || open_count > 16)
		{
			console::warn("[menu - reload] invalid native UI counts; keeping current menus\n");
			return false;
		}
		const auto is_custom = [](game::menuDef_t* menu)
		{
			return std::any_of(loaded_menus.begin(), loaded_menus.end(), [menu](const auto& entry) { return entry.second == menu; });
		};
		for (int i = 0; i < open_count; ++i)
		{
			if (is_custom(open_menus[i]))
			{
				console::warn("[menu - reload] close menu '%s' before reloading; keeping current menus\n", open_menus[i]->window.name);
				return false;
			}
		}
		int retained = 0;
		for (int i = 0; i < count; ++i)
		{
			if (!is_custom(menus[i])) menus[retained++] = menus[i];
		}
		std::fill(menus + retained, menus + count, nullptr);
		count = retained;
		loaded_menus.clear();
		loaded_menu_sources.clear();
		scanned = false;
		ensure_loaded();
		return true;
	}

	game::menuDef_t* find(const std::string& name)
	{
		ensure_loaded();
		const auto it = loaded_menus.find(lower(name));
		return it == loaded_menus.end() ? nullptr : it->second;
	}

	const std::unordered_map<std::string, game::menuDef_t*>& all()
	{
		ensure_loaded();
		return loaded_menus;
	}
#endif
}

#ifndef CONSOLATION_MENU_PARSER_TEST
REGISTER_COMPONENT(menu_file::component)
#endif
