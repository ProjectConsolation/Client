#include <std_include.hpp>

#ifndef CONSOLATION_MENU_PARSER_TEST
#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "filesystem.hpp"
#include "menu_file.hpp"
#include "../renderer/menu_image.hpp"

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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "menu_slideshow.hpp"
#include "menu_loadout.hpp"

namespace menu_file
{
	namespace
	{
		// Native String_Parse disallows line breaks. Disk scripts need the same
		// single-line, separated-token representation as compiled menu assets.
		std::string normalize_script(const std::string& source)
		{
			std::string result;
			bool quoted = false;
			for (std::size_t i = 0; i < source.size(); ++i)
			{
				const char c = source[i];
				if (quoted)
				{
					result += c;
					if (c == '\\' && i + 1 < source.size()) result += source[++i];
					else if (c == '"') quoted = false;
					continue;
				}
				if (c == '"') { quoted = true; result += c; }
				else if (source.compare(i, 2, "//") == 0)
				{
					while (i + 1 < source.size() && source[i + 1] != '\n') ++i;
					result += ' ';
				}
				else if (source.compare(i, 2, "/*") == 0)
				{
					i = source.find("*/", i + 2);
					if (i == std::string::npos) break; // tokenizer already rejects this
					++i;
					result += ' ';
				}
				else if (c == ';') result += " ; ";
				else result += std::isspace(static_cast<unsigned char>(c)) ? ' ' : c;
			}
			return result;
		}
		// Expected unsupported input is a warning, not a thrown exception during
		// automatic discovery. Do not strip directives and publish half a menu.
		// Match tokenizer's handling of strings/comments, including action blocks.
		int unsupported_directive_line(const std::string& source)
		{
			enum class state { normal, quoted, line_comment, block_comment };
			auto mode = state::normal;
			int line = 1;
			for (std::size_t i = 0; i < source.size(); ++i)
			{
				const char c = source[i];
				const char next = i + 1 < source.size() ? source[i + 1] : '\0';
				if (c == '\n') ++line;
				if (mode == state::line_comment)
				{
					if (c == '\n') mode = state::normal;
					continue;
				}
				if (mode == state::block_comment)
				{
					if (c == '*' && next == '/') { mode = state::normal; ++i; }
					continue;
				}
				if (mode == state::quoted)
				{
					if (c == '\\' && next) { if (next == '\n') ++line; ++i; }
					else if (c == '"') mode = state::normal;
					continue;
				}
				if (c == '/' && next == '/') { mode = state::line_comment; ++i; }
				else if (c == '/' && next == '*') { mode = state::block_comment; ++i; }
				else if (c == '"') mode = state::quoted;
				else if (c == '#') return line;
			}
			return 0;
		}

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

			// Read balanced source, then serialize a single-line script for QoS.
			// Braces inside comments and strings are not delimiters.
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

				return normalize_script(source_.substr(start, (cursor_ - 1) - start));
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
			std::string background;
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
			std::vector<std::pair<int, std::string>> key_actions;
			std::string mouse_enter, mouse_exit, mouse_enter_text, mouse_exit_text;
			std::string dvar, dvar_test, enable_dvar;

			bool has_listbox = false;
			float lb_element_width = 0, lb_element_height = 0;
			int lb_element_style = 0;
			float lb_feeder = 0;
			std::vector<std::array<int, 4>> lb_columns;
			std::string lb_double_click;
			std::array<float, 4> lb_select_border{0.80f, 0.025f, 0.065f, 1.0f};

			bool has_editfield = false;
			float ef_min = 0, ef_max = 0, ef_def = 0, ef_range = 0;
			int ef_max_chars = 0, ef_max_chars_goto_next = 0, ef_max_paint_chars = 0;
		};

		struct parsed_menu
		{
			std::string name;
			std::string background;
			std::string overlay_for, scaleform_menu;
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

		// Verified QoS Item_Text_Paint 102CE710 and CL_GetFontForMode
		// 102D9130; compare KisakCOD UI_GetFontHandle. These are modes,
		// not arbitrary asset registrations or TTF filename indexes.
		int resolve_font_enum(tokenizer& tok, const std::string& name)
		{
			const auto normalized = lower(name);
			if (normalized == "bigfont" || normalized == "fonts/bigfont") return 2;
			if (normalized == "objectivefont" || normalized == "fonts/objectivefont") return 6;
			if (normalized == "consolefont" || normalized == "fonts/consolefont") return 5;
			if (normalized == "boldfont" || normalized == "fonts/boldfont") return 4;
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
				{"background", [](tokenizer& t, parsed_item& i) { i.background = t.next(); }},
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
				{"execkeyint", [](tokenizer& t, parsed_item& i) {
					const int key = t.integer();
					if (key < 0 || key > 255) t.fail("execKeyInt expects a native key code from 0 to 255");
					i.key_actions.emplace_back(key, t.read_balanced_block());
				}},
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
				{"feeder", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_feeder = t.number(); }},
				{"columns", [](tokenizer& t, parsed_item& i) {
					i.has_listbox = true;
					const auto count = t.integer();
					if (count < 0 || count > 16) t.fail("columns must contain 0 to 16 entries");
					i.lb_columns.clear();
					for (int column = 0; column < count; ++column)
						i.lb_columns.push_back({t.integer(), t.integer(), t.integer(), t.integer()});
				}},
				{"doubleclick", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_double_click = t.read_balanced_block(); }},
				{"selectborder", [](tokenizer& t, parsed_item& i) { i.has_listbox = true; i.lb_select_border = read_color4(t); }},
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
				{"background", [](tokenizer& t, parsed_menu& m) { m.background = t.next(); }},
				{"overlayfor", [](tokenizer& t, parsed_menu& m) { m.overlay_for = t.next(); }},
				{"scaleformmenu", [](tokenizer& t, parsed_menu& m) { m.scaleform_menu = t.next(); }},
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
			if (!menu.overlay_for.empty())
			{
				if (!menu.on_open.empty() || !menu.on_close.empty() || !menu.on_esc.empty() || menu.full_screen)
					tok.fail("display-only overlays cannot have menu actions or fullscreen");
				for (const auto& item : menu.items)
					if (!item.decoration || item.type != 0 || item.background.empty()
						|| item.rect.w <= 0 || item.rect.h <= 0
						|| (item.rect.horz_align != 1 && item.rect.horz_align != 3) || item.rect.vert_align != 1
						|| !item.text.empty() || !item.action.empty() || !item.on_focus.empty()
						|| !item.on_accept.empty() || !item.leave_focus.empty()
						|| !item.mouse_enter.empty() || !item.mouse_exit.empty()
						|| !item.mouse_enter_text.empty() || !item.mouse_exit_text.empty() || !item.dvar.empty())
						tok.fail("overlays accept only decorative images with left/right, top-aligned rectangles");
			}
			else if (!menu.scaleform_menu.empty()) tok.fail("scaleformMenu requires overlayFor");

			return menu;
		}

		std::vector<parsed_menu> parse_menu_file(const std::string& source)
		{
			tokenizer tok(source);
			std::vector<parsed_menu> menus;
			bool root = false;
			while (!tok.at_end())
			{
				const auto field = tok.next();
				if (field.empty())
				{
					break;
				}

				if (field == "{" && !root) { root = true; continue; }
				if (field == "}" && root) { root = false; continue; }
				if (lower(field) != "menudef")
				{
					tok.fail(std::format("expected 'menuDef', got '{}'", field));
				}

				menus.push_back(parse_menu(tok));
			}
			if (root) tok.fail("unterminated menu file root");

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
			// KisakCOD Item_SetFocus; QoS PC 102D15C0 checks this exact bit.
			item->window.staticFlags = parsed.decoration ? 0x100000 : 0;
			// QoS Window_Paint 102D1A00 consumes 0x10000 for explicit foreColor.
			std::fill(std::begin(item->window.dynamicFlags), std::end(item->window.dynamicFlags), 0x10000 | (parsed.visible ? 4 : 0));
			std::copy(parsed.fore_color.begin(), parsed.fore_color.end(), item->window.foreColor);
			std::copy(parsed.back_color.begin(), parsed.back_color.end(), item->window.backColor);
			std::copy(parsed.border_color.begin(), parsed.border_color.end(), item->window.borderColor);
			if (!parsed.background.empty() && !lower(parsed.background).ends_with(".png"))
				item->window.background = game::Material_RegisterHandle(parsed.background.c_str());

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
			// QoS Menu_HandleKey 102D7210 consults onKey before generic Enter.
			// onAccept is for edit fields; listbox action also runs on arrows.
			for (auto key = parsed.key_actions.rbegin(); key != parsed.key_actions.rend(); ++key)
			{
				auto* handler = utils::memory::allocate<game::ItemKeyHandler>();
				handler->key = key->first;
				handler->action = allocate_menu_string(key->second);
				handler->next = item->onKey;
				item->onKey = handler;
			}
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
				// Both paint and input validate dataType == 6 before using the union.
				item->dataType = 6;
				auto* listbox = utils::memory::allocate<game::listBoxDef_s>();
				std::memset(listbox, 0, sizeof(*listbox));
				listbox->elementWidth = parsed.lb_element_width;
				listbox->elementHeight = parsed.lb_element_height;
				listbox->elementStyle = parsed.lb_element_style;
				listbox->numColumns = static_cast<int>(parsed.lb_columns.size());
				for (int column = 0; column < listbox->numColumns; ++column)
				{
					const auto& value = parsed.lb_columns[column];
					listbox->columnInfo[column] = {value[0], value[1], value[2], value[3]};
				}
				listbox->doubleClick = allocate_menu_string_opt(parsed.lb_double_click);
				std::copy(parsed.lb_select_border.begin(), parsed.lb_select_border.end(), listbox->selectBorder);
				item->typeData.listBox = listbox;
			}
			else if (parsed.has_editfield)
			{
				item->dataType = 4;
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
		std::unordered_map<std::string, parsed_menu> loaded_overlays;
		std::unordered_map<std::string, std::string> loaded_menu_sources;
		// Keep asset names separate from native material pointers; PNG upload is
		// deferred until the renderer exists, and donor techniques refresh per paint.
		struct png_background { std::string filename; int style; };
		std::unordered_map<game::windowDef_t*, png_background> png_backgrounds;
		bool frontend_transition = false;
		bool frontend_was_away = false;
		bool frontend_return_requested = false;
		bool frontend_match_active = false;
		bool local_loadout_active = false;
		std::string pending_scaleform_screen;
		std::string return_frontend_page;
		bool scanned = false;
		utils::hook::detour open_menu_hook;
		utils::hook::detour paint_menus_hook;
		utils::hook::detour frontend_screen_hook;
		utils::hook::detour window_paint_hook;
		utils::hook::detour stock_guide_hook;
		utils::hook::detour localized_message_hook;

		int __cdecl open_discord()
		{
			const auto result = ShellExecuteA(nullptr, "open", "https://discord.gg/XSrTvXJcsw", nullptr, nullptr, SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(result) <= 32)
			{
				console::warn("Could not open the Discord invite in your browser.\n");
				return 0;
			}
			return 1;
		}

		const char* __cdecl localized_message_stub(const char* source, const char* context, const int message_type)
		{
			// QoS SFStringLocalizer::Translate (10002370) strips '@' and calls
			// 103CA030 with three caller-clean stack arguments. Preserve native
			// localization/formatting everywhere except these stock frontend strings.
			// The extracted PC menuData pairs MENU_WLGUIDE with xshowguideui.
			if (source && context && !std::strcmp(context, "scaleform translator"))
			{
				if (!std::strcmp(source, "MENU_WLGUIDE")) return "DISCORD";
				if (!std::strcmp(source, "MENU_WLGUIDE_INSTR")) return "Join the Discord for Project: Consolation updates!";
				if (!std::strcmp(source, "XBOXLIVE_XBOXLIVE")) return "PLAY ONLINE";
				if (!std::strcmp(source, "XBOXLIVE_XBOXLIVE_INSTR")) return "Play online multiplayer.";
			}
			return localized_message_hook.invoke<const char*>(source, context, message_type);
		}
		std::unordered_map<std::string, std::vector<std::string>> preview_lists;

		const std::vector<std::string>& slideshow_files(const std::string& filename)
		{
			const auto prefix = filename == "slideshow_mp.png" ? "preview_mp_" : "preview_sp_";
			if (!preview_lists.contains(filename))
			{
				auto& files = preview_lists[filename];
				for (const auto& root : filesystem::get_search_paths())
				{
					std::error_code error;
					std::filesystem::directory_iterator it(std::filesystem::path(root) / "images", error), end;
					for (; !error && it != end; it.increment(error))
					{
						const auto name = it->path().filename().string();
						if (it->is_regular_file(error) && name.starts_with(prefix) && name.ends_with(".png")) files.push_back(name);
					}
				}
				std::sort(files.begin(), files.end());
				files.erase(std::unique(files.begin(), files.end()), files.end());
			}
			return preview_lists.at(filename);
		}
		bool is_slideshow(const std::string& filename)
		{
			return filename == "slideshow_mp.png" || filename == "slideshow_sp.png";
		}
        game::Material* menu_background(const std::string& filename)
        {
            if (filename == "lobby_map.png")
            {
                const auto* map = game::Dvar_FindVar("ui_mapname");
                if (map && static_cast<unsigned char>(map->type) == game::DVAR_TYPE_STRING && map->current.string)
                {
                    const std::string name(map->current.string);
                    if (name.starts_with("mp_") && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos)
                        if (auto* image = menu_image::get("preview_mp_" + name + ".png")) return image;
                }
                const auto& files = slideshow_files("slideshow_mp.png");
                return files.empty() ? nullptr : menu_image::get(files.front());
            }
			if (!is_slideshow(filename)) return menu_image::get(filename);
			const auto& files = slideshow_files(filename);
			return files.empty() ? nullptr : menu_image::get(files.front());
		}

		void draw_slide(unsigned char* context, game::windowDef_t* window,
			const std::string& filename, std::size_t index, float progress, float alpha)
		{
            auto* material = filename == "lobby_map.png" ? menu_background(filename) : menu_image::get(filename);
			if (!material || !material->textureTable || alpha <= 0) return;
			const auto* image = static_cast<menu_image::image*>(material->textureTable[0].image);
			if (!image || !image->height || window->rect.w <= 0 || window->rect.h <= 0) return;
			auto rect = window->rect;
			// QoS Window_Paint 102D1A00 uses per-client ScrPlaceView (76 bytes).
			// ScrPlace_ApplyRect 1030DF40: EDX=x, ECX=w, EDI=y, ESI=h;
			// stack=(placement, horizontal alignment, vertical alignment).
			const auto client = *reinterpret_cast<const int*>(context);
			if (client < 0 || client > 3) return;
			const auto placement = game::game_offset(0x1127BA50) + client * 76;
			const auto target = game::game_offset(0x1030DF40);
			auto* x = &rect.x; auto* y = &rect.y; auto* w = &rect.w; auto* h = &rect.h;
			const int horizontal = rect.horzAlign, vertical = rect.vertAlign;
			__asm
			{
				push esi
				push edi
				push vertical
				push horizontal
				push placement
				mov edx, x
				mov ecx, w
				mov edi, y
				mov esi, h
				call target
				add esp, 12
				pop edi
				pop esi
			}
			if (rect.w <= 0 || rect.h <= 0) return;
			const auto uv = menu_slideshow::cover(static_cast<float>(image->width) / image->height,
				rect.w / rect.h, index, progress);
			float color[4]{window->foreColor[0], window->foreColor[1], window->foreColor[2], window->foreColor[3] * alpha};
			game::R_AddCmdDrawStretchPic(rect.x, rect.y, rect.w, rect.h,
				uv.s0, uv.t0, uv.s1, uv.t1, color, material, 0);
		}

		void __cdecl window_paint_stub(unsigned char* context, game::windowDef_t* window,
			float fade_amount, float fade_in, float fade_clamp, int fade_cycle)
		{
			const auto binding = png_backgrounds.find(window);
            if (binding != png_backgrounds.end() && binding->second.filename == "lobby_map.png")
            {
                static const auto start = GetTickCount64();
                draw_slide(context, window, "lobby_map.png", 0,
                    menu_slideshow::background_progress(GetTickCount64() - start), 1);
                const auto style = window->style;
                window->style = 0;
                window_paint_hook.invoke<void>(context, window, fade_amount, fade_in, fade_clamp, fade_cycle);
                window->style = style;
                return;
            }
            if (binding != png_backgrounds.end() && is_slideshow(binding->second.filename))
			{
				const auto& files = slideshow_files(binding->second.filename);
				if (!files.empty())
				{
					static const auto start = GetTickCount64();
					const auto frame = menu_slideshow::sample(GetTickCount64() - start, files.size());
					// Opaque outgoing layer + fading incoming layer avoids a black dip.
					draw_slide(context, window, files[frame.previous], frame.previous, frame.previous_progress, 1);
					draw_slide(context, window, files[frame.current], frame.current, frame.progress, frame.alpha);
				}
				const auto style = window->style;
				window->style = 0;
				window_paint_hook.invoke<void>(context, window, fade_amount, fade_in, fade_clamp, fade_cycle);
				window->style = style;
				return;
			}
			window_paint_hook.invoke<void>(context, window, fade_amount, fade_in, fade_clamp, fade_cycle);
		}
		bool scaleform_suppressed = false;
		bool previous_scaleform_enabled = true;

		void set_scaleform_enabled(game::dvar_s* dvar, const bool enabled)
		{
			// QoS PC Dvar_SetBool 10274BA0: ESI=dvar, stack=(bool, source).
			const auto target = game::game_offset(0x10274BA0);
			const int value = enabled;
			__asm
			{
				push esi
				mov esi, dvar
				push 0 // DVAR_SOURCE_INTERNAL
				push value
				call target
				add esp, 8
				pop esi
			}
		}

		void restore_scaleform(const bool required_by_destination = false)
		{
			if (!scaleform_suppressed) return;
			if (auto* dvar = game::Dvar_FindVar("sf_enable"))
				set_scaleform_enabled(dvar, required_by_destination || previous_scaleform_enabled);
			scaleform_suppressed = false;
		}

		void __cdecl frontend_screen_stub(const char* name)
		{
			// UI_SetActiveMenu calls 102DC580 on return to the frontend too.
			// Reset the pending route here; sf_current_menu is an AS callback and
			// can still contain the old screen while this transition is pending.
			frontend_transition = !name || _stricmp(name, "mpmainmenu") != 0;
			frontend_return_requested = !frontend_transition;
			if (frontend_return_requested)
			{
				pending_scaleform_screen.clear();
				frontend_match_active = false;
			}
			restore_scaleform(frontend_transition);
			frontend_screen_hook.invoke<void>(name);
		}

		bool register_native_menu(unsigned char* context, game::menuDef_t* menu)
		{
			auto& count = *reinterpret_cast<int*>(context + 2104);
			auto** menus = reinterpret_cast<game::menuDef_t**>(context + 56);
			if (count < 0 || count > 512) return false;
			for (int i = 0; i < count; ++i)
				if (menus[i] == menu) return true;
			if (count == 512) return false;
			menus[count++] = menu;
			return true;
		}

		void update_frontend(unsigned char* context)
		{
			const auto client = *reinterpret_cast<const int*>(context);
			const auto count = *reinterpret_cast<const int*>(context + 2172);
			if (client < 0 || client > 3 || count < 0 || count > 16) { restore_scaleform(); return; }
			// QoS CL_UpdateInGameState (1031CC50) reads connection state at
			// 111F45F8: active=10; connect (10311330) enters state 3. Like
			// KisakCOD, cl_ingame only becomes true after loading completes.
			// sf_current_menu can remain mpsyssetup throughout the entire match;
			// never use that stale frontend screen to reopen a lobby over gameplay.
			const auto connection_state = reinterpret_cast<const int*>(game::game_offset(0x111F45F8))[client];
			if (connection_state >= 3) frontend_match_active = true;
			if (frontend_match_active)
			{
				std::array<game::menuDef_t*, 16> open{};
				std::copy_n(reinterpret_cast<game::menuDef_t**>(context + 2108), count, open.begin());
				bool closed = false;
				for (int i = count - 1; i >= 0; --i)
				{
					auto* menu = open[i];
					if (menu && menu->window.name && std::string_view(menu->window.name).starts_with("csl_"))
					{
						utils::hook::invoke<void>(game::game_offset(0x102CE070), context, menu);
						closed = true;
					}
				}
				// Native connection entry releases menu capture too (10311593).
				if (closed) *game::keyCatchers &= ~0x10;
				pending_scaleform_screen.clear();
				return_frontend_page.clear();
				frontend_return_requested = false;
				restore_scaleform();
				return;
			}
			const auto* screen = game::Dvar_FindVar("sf_current_menu");
			if (!screen || static_cast<unsigned char>(screen->type) != game::DVAR_TYPE_STRING || !screen->current.string) return;
			// A queued sf_open can leave the previous screen reported for a frame.
			// Do not reopen that replacement and disable GFx before it processes
			// the requested destination (especially host -> settings transitions).
			if (!pending_scaleform_screen.empty())
			{
				if (_stricmp(screen->current.string, pending_scaleform_screen.c_str()))
				{
					restore_scaleform(true);
					return;
				}
				pending_scaleform_screen.clear();
				frontend_transition = false;
			}
			const bool root = !_stricmp(screen->current.string, "mpmainmenu");
			const char* replacement_name = root ? "csl_main"
				: !_stricmp(screen->current.string, "mpsyssetup") ? "csl_host"
				: !_stricmp(screen->current.string, "mpxblobbyprivatematch") ? "csl_private"
				: !_stricmp(screen->current.string, "mpxblivemain") ? "csl_online"
				: !_stricmp(screen->current.string, "mpsysjoin") ? "csl_serverbrowser" : nullptr;
			if (replacement_name && local_loadout_active)
			{
				// The LIVE wrapper normally clears this on exit; our direct local
				// editor route bypasses that wrapper, so balance it on return.
				command::execute("set modifyingloadouts 0\n");
				local_loadout_active = false;
			}
			const bool returned = root && frontend_was_away;
			if (!root) { frontend_transition = false; frontend_was_away = true; }
			else if (returned) { frontend_transition = false; frontend_was_away = false; }
			auto* top = count ? reinterpret_cast<game::menuDef_t**>(context + 2108)[count - 1] : nullptr;
			const bool native_main = top && top->window.name && !_stricmp(top->window.name, "main")
				&& (top->window.dynamicFlags[client] & 4);
			if (replacement_name && !frontend_transition && (!root || native_main || returned || frontend_return_requested))
			{
				auto* replacement = find(replacement_name);
				if (!replacement || count == 16) return;
				// Register companion dialogs before onOpen/actions invoke native open.
				for (const auto& [name, menu] : loaded_menus)
					if (name.starts_with("csl_")) register_native_menu(context, menu);
				if (register_native_menu(context, replacement))
				{
					// An explicit return may arrive after the root is already open;
					// don't reopen it over a confirmation dialog or reset its focus.
					bool present = false;
					for (int i = 0; i < count; ++i)
						present |= reinterpret_cast<game::menuDef_t**>(context + 2108)[i] == replacement;
					if (!present) utils::hook::invoke<int>(game::game_offset(0x102D8B90), context, replacement->window.name);
					if (root && !return_frontend_page.empty())
					{
						if (auto* page = find(return_frontend_page))
							utils::hook::invoke<int>(game::game_offset(0x102D8B90), context, page->window.name);
						return_frontend_page.clear();
					}
					frontend_return_requested = false;
					*game::keyCatchers |= 0x10;
				}
			}
			else if (top && top->window.name
				&& (!_stricmp(top->window.name, "csl_main") || !_stricmp(top->window.name, "csl_host") || !_stricmp(top->window.name, "csl_private") || !_stricmp(top->window.name, "csl_serverbrowser"))
				&& (!replacement_name || _stricmp(top->window.name, replacement_name)))
				utils::hook::invoke<void>(game::game_offset(0x102CE070), context, top);
			bool custom_frontend = false;
			for (int i = 0; i < *reinterpret_cast<int*>(context + 2172); ++i)
				custom_frontend |= replacement_name && reinterpret_cast<game::menuDef_t**>(context + 2108)[i] == find(replacement_name);
			if (custom_frontend && !frontend_transition)
			{
				if (auto* dvar = game::Dvar_FindVar("sf_enable"))
				{
					if (!scaleform_suppressed) previous_scaleform_enabled = dvar->current.enabled;
					scaleform_suppressed = true;
					set_scaleform_enabled(dvar, false);
				}
			}
			else restore_scaleform();
		}

		void paint_menus_stub(unsigned char* context)
		{
			ensure_loaded();
			update_frontend(context);
			for (const auto& [window, binding] : png_backgrounds)
			{
				window->background = menu_background(binding.filename);
				// Shader-style Window_Paint does not guard a null material.
				window->style = window->background ? binding.style : 0;
			}
			// QoS 102D839C / KisakCOD Menu_PaintAll starts at the last
			// fullscreen menu. Keep the stock host's video paint, but do not
			// change fullscreen input handling or draw older custom pages below
			// a new page. Restore all fields before the next input/frame pass.
			struct paint_state { game::menuDef_t* menu; int fullscreen; int flags; };
			std::array<paint_state, 16> saved{};
			int saved_count = 0, latest_page = -1;
			const auto stack_count = *reinterpret_cast<const int*>(context + 2172);
			const auto local_client = *reinterpret_cast<const int*>(context);
			if (stack_count >= 0 && stack_count <= 16 && local_client >= 0 && local_client < 4)
			{
				for (int i = 0; i < stack_count; ++i)
				{
					auto* page = reinterpret_cast<game::menuDef_t**>(context + 2108)[i];
					if (page && page->window.name && std::string_view(page->window.name).starts_with("csl_") && page->fullScreen)
					{
						saved[saved_count++] = {page, page->fullScreen, page->window.dynamicFlags[local_client]};
						page->fullScreen = 0;
						latest_page = saved_count - 1;
					}
				}
				for (int i = 0; i < latest_page; ++i) saved[i].menu->window.dynamicFlags[local_client] &= ~4u;
			}
            // Menu_Open clears the stock main host's visibility when opening a
            // fullscreen replacement. Its native video therefore disappears on
            // root/navigation pages, although it remains visible in host setup.
            // Paint the host only during this pass; never give it input focus.
            game::menuDef_t* video_host = nullptr;
            int video_flags = 0;
            int video_static_flags = 0;
            int video_visible_entries = 0;
            if (latest_page >= 0)
            {
                const auto registered = *reinterpret_cast<const int*>(context + 2104);
                if (registered >= 0 && registered <= 512)
                    for (int i = 0; i < registered; ++i)
                    {
                        auto* candidate = reinterpret_cast<game::menuDef_t**>(context + 56)[i];
                        if (candidate && candidate->window.name && !_stricmp(candidate->window.name, "main"))
                        {
                            video_host = candidate;
                            video_flags = candidate->window.dynamicFlags[local_client];
                            video_static_flags = candidate->window.staticFlags;
                            video_visible_entries = candidate->visibleExp.numEntries;
                            // QoS Menu_IsVisible 102CE890 (also KisakCOD): a
                            // KEYCATCH_UI-hidden host remains hidden despite
                            // WINDOW_VISIBLE. Only bypass that during painting.
                            candidate->window.staticFlags &= ~0x40000000;
                            candidate->visibleExp.numEntries = 0;
                            candidate->window.dynamicFlags[local_client] |= 4;
                            break;
                        }
                    }
            }
            paint_menus_hook.invoke<void>(context);
            if (video_host)
            {
                video_host->window.dynamicFlags[local_client] =
                    (video_host->window.dynamicFlags[local_client] & ~4) | (video_flags & 4);
                video_host->window.staticFlags = video_static_flags;
                video_host->visibleExp.numEntries = video_visible_entries;
            }
			for (int i = 0; i < saved_count; ++i)
			{
				saved[i].menu->fullScreen = saved[i].fullscreen;
				if (i < latest_page)
				{
					// Only restore the temporary visibility mask, preserving any
					// other native paint/fade bookkeeping performed this frame.
					auto& flags = saved[i].menu->window.dynamicFlags[local_client];
					flags = (flags & ~4) | (saved[i].flags & 4);
				}
			}
			// QoS PC 1.1 Menu_PaintAll 102D8290: stack +2108, count +2172.
			// Unlike openmenu, this path never registers, focuses or stacks an overlay.
			const auto count = *reinterpret_cast<const int*>(context + 2172);
			const auto client = *reinterpret_cast<const int*>(context);
			if (count <= 0 || count > 16 || client < 0 || client > 3) return;
			auto* menu = reinterpret_cast<game::menuDef_t**>(context + 2108)[count - 1];
			if (!menu || !menu->window.name || !(menu->window.dynamicFlags[client] & 4)) return;
			const auto* screen = game::Dvar_FindVar("sf_current_menu");
			const auto width = *reinterpret_cast<const unsigned*>(game::game_offset(0x10E27190));
			const auto height = *reinterpret_cast<const unsigned*>(game::game_offset(0x10E27194));
			if (!width || !height) return;
			const float scale = static_cast<float>(height) / 480.0f;
			for (const auto& [name, overlay] : loaded_overlays)
			{
				if (!overlay.visible || _stricmp(menu->window.name, overlay.overlay_for.c_str())) continue;
				// sf_current_menu is consumed by QoS 1040EA50 for frontend navigation.
				if (!overlay.scaleform_menu.empty() && (!screen || static_cast<unsigned char>(screen->type) != game::DVAR_TYPE_STRING
					|| !screen->current.string || _stricmp(screen->current.string, overlay.scaleform_menu.c_str()))) continue;
				for (const auto& item : overlay.items)
				{
					if (!item.visible) continue;
					auto* material = menu_image::get(item.background);
					if (!material) continue;
					const auto x = item.rect.x * scale + (item.rect.horz_align == 3 ? static_cast<float>(width) : 0.0f);
					auto color = item.fore_color;
					game::R_AddCmdDrawStretchPic(x, item.rect.y * scale, item.rect.w * scale, item.rect.h * scale,
						0, 0, 1, 1, color.data(), material, 0);
				}
			}
		}

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

			const auto directive_line = unsupported_directive_line(source);
			if (directive_line)
			{
				console::warn("[menu - disk] skipped %s: line %d: #include/#define preprocessing is not implemented; file left unchanged\n",
					disk_path.string().c_str(), directive_line);
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
				if (loaded_menus.contains(key) || loaded_overlays.contains(key))
				{
					console::warn("[menu - disk] duplicate menu name '%s' in %s, keeping first definition\n",
						parsed.name.c_str(), disk_path.string().c_str());
					continue;
				}
				if (!parsed.overlay_for.empty())
				{
					loaded_overlays.emplace(key, parsed);
					console::info("[menu - overlay] loaded '%s' for %s (display-only)\n", parsed.name.c_str(), parsed.overlay_for.c_str());
					continue;
				}

				auto* native = build_native_menu(parsed);
				if (lower(parsed.background).ends_with(".png"))
					png_backgrounds.emplace(&native->window, png_background{parsed.background, parsed.style});
				else if (!parsed.background.empty()) native->window.background = game::Material_RegisterHandle(parsed.background.c_str());
				for (std::size_t i = 0; i < parsed.items.size(); ++i)
					if (lower(parsed.items[i].background).ends_with(".png"))
					{
						png_backgrounds.emplace(&native->items[i]->window, png_background{parsed.items[i].background, parsed.items[i].style});
						native->items[i]->window.style = 0;
					}
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
			std::unordered_set<std::string> visited;
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
						const auto resolved = std::filesystem::weakly_canonical(entry.path(), error);
						if (error) { error.clear(); continue; }
						if (!visited.insert(lower(resolved.generic_string())).second) continue;
						parse_and_register(entry.path());
					}
				}
			}
		}

		void register_commands()
		{
			command::add("cslMenuRoute", [](const command::params& args)
			{
				if (args.size() != 2) { console::info("usage: cslMenuRoute <options|host|join|online|private|loadout|stats|map|mode|rules|main|browserback|leave|discord>\n"); return; }
				const auto route = lower(args.get(1));
				if (route == "discord")
				{
					open_discord();
					return;
				}
				if (route == "map" || route == "mode" || route == "browserback")
				{
					auto* context = reinterpret_cast<unsigned char*>(game::game_offset(0x113CFC38));
					const auto* name = route == "map" ? "csl_maps" : route == "mode" ? "csl_modes" : "csl_systemlink";
					if (route == "browserback")
					{
						if (auto* browser = find("csl_serverbrowser"))
							utils::hook::invoke<void>(game::game_offset(0x102CE070), context, browser);
						// Leave the browser's native screen too; otherwise update_frontend
						// immediately reopens its replacement on the next frame.
						pending_scaleform_screen = "mpmainmenu";
						return_frontend_page = "csl_systemlink";
						frontend_return_requested = true;
						frontend_transition = false;
						restore_scaleform(true);
						command::execute("sf_open mpmainmenu\n");
						return;
					}
					if (auto* menu = find(name); menu && register_native_menu(context, menu))
						utils::hook::invoke<int>(game::game_offset(0x102D8B90), context, name);
					return;
				}
				std::string script;
				if (route == "options") script = "sf_open cmoptions\n";
				else if (route == "loadout")
				{
					auto* context = reinterpret_cast<unsigned char*>(game::game_offset(0x113CFC38));
					const auto count = *reinterpret_cast<const int*>(context + 2172);
					const char* lobby = nullptr;
					// Settings sits above its owning lobby. Inspect the stack rather
					// than onlinegame, which is not set merely by opening ONLINE PLAY.
					for (int i = count >= 0 && count <= 16 ? count - 1 : -1; i >= 0; --i)
					{
						const auto* menu = reinterpret_cast<game::menuDef_t**>(context + 2108)[i];
						if (!menu || !menu->window.name) continue;
						const std::string_view name(menu->window.name);
						if (menu_loadout::is_lobby(name))
						{
							lobby = menu->window.name;
							break;
						}
					}
					if (lobby) return_frontend_page = lobby;
					if (lobby && std::string_view(lobby) == "csl_host")
					{
						local_loadout_active = true;
						// QoS menuData: mploadout is the LIVE-gated shop wrapper;
						// mpcustomloadout is its native class editor. Keep the local
						// session intact; do not sign into LIVE or start an online party.
					}
					script = menu_loadout::script(lobby ? std::string_view(lobby) : std::string_view{});
				}
				else if (route == "stats") script = "sf_open mpaccompleads\n";
				else if (route == "rules") script = "sf_open mpsysoptions\n";
				else if (route == "host" || route == "join")
					// QoS menuData mpsyslink onClickExec setup, followed by the
					// native sf_open command (1040DE30), not a guessed GFx ABI.
					script = "nosplitscreen; xsignin; systemlink 1; splitscreen 0; xblive_rankedmatch 0; onlinegame 0; exec default_systemlink.cfg; set ui_mptype 1; sf_open "
						+ std::string(route == "host" ? "mpsyssetup\n" : "mpsysjoin\n");
				else if (route == "online")
					script = "nosplitscreen; set xsigninscreen mpxbplaylistselect; xsigninlive; systemlink 0; splitscreen 0; onlinegame 1; exec default_xboxlive.cfg; party_maxplayers 12; xblive_privatematch 0; xblive_rankedmatch 0; party_timerVisible 0; xstartprivateparty; sf_open mpxbplaylistselect\n";
				else if (route == "private")
					// Extracted QoS menuData onClickExec, not a fabricated party.
					script = "nosplitscreen; set xsigninscreen mpxblobbyprivatematch; xsigninlive; systemlink 0; splitscreen 0; onlinegame 1; exec default_xboxlive.cfg; xblive_rankedmatch 0; ui_enumeratesaved; party_timerVisible 0; xblive_privatematch 1; xstartprivateparty; xstartpartyhost; sf_open mpxblobbyprivatematch\n";
				else if (route == "main") script = "sf_open mpmainmenu\n";
				else if (route == "leave")
				{
					const auto* online = game::Dvar_FindVar("onlinegame");
					return_frontend_page = online && static_cast<unsigned char>(online->type) == game::DVAR_TYPE_BOOL && online->current.enabled
						? "csl_online" : "csl_systemlink";
					// QoS menuData leaves the match lobby before stopping its parties.
					if (online && static_cast<unsigned char>(online->type) == game::DVAR_TYPE_BOOL && online->current.enabled)
						script = "party_leavematchlobby; onlinegame 0; xstopprivateparty; xstopparty; ";
					script += "sf_open mpmainmenu\n";
				}
				else { console::warn("cslMenuRoute: unknown destination '%s'\n", route.c_str()); return; }
				const auto destination = script.rfind("sf_open ");
				pending_scaleform_screen = script.substr(destination + 8);
				if (!pending_scaleform_screen.empty() && pending_scaleform_screen.back() == '\n') pending_scaleform_screen.pop_back();
				auto* context = reinterpret_cast<unsigned char*>(game::game_offset(0x113CFC38));
				for (const auto* name : {"csl_maps", "csl_modes", "csl_settings", "csl_systemlink", "csl_online", "csl_host", "csl_private", "csl_serverbrowser", "csl_main"})
					if (auto* menu = find(name)) utils::hook::invoke<void>(game::game_offset(0x102CE070), context, menu);
				frontend_transition = route != "main" && route != "leave";
				frontend_return_requested = !frontend_transition;
				restore_scaleform(true);
				if (auto* enabled = game::Dvar_FindVar("sf_enable")) set_scaleform_enabled(enabled, true);
				command::execute(script);
			});
			command::add("reloadMenus", [](const command::params&)
			{
				if (reload()) console::info("reloadMenus: %zu custom menu(s), %zu display-only overlay(s) loaded\n", loaded_menus.size(), loaded_overlays.size());
			});
		}

		class component final : public component_interface
		{
		public:
			void post_load() override
			{
				// QoS PC 1.1 registers this no-argument cdecl command callback at
				// 10310724. Replace the menu action, not the XLive API underneath.
				// Retire these adapters if the stock movie is intentionally replaced.
				stock_guide_hook.create(game::game_offset(0x1030E7E0), open_discord);
				localized_message_hook.create(game::game_offset(0x103CA030), localized_message_stub);
				open_menu_hook.create(game::game_offset(0x102E1560), open_menu_stub);
				paint_menus_hook.create(game::game_offset(0x102D8290), paint_menus_stub);
				frontend_screen_hook.create(game::game_offset(0x102DC580), frontend_screen_stub);
				// QoS PC 1.1 / KisakCOD Window_Paint comparison; only disk slideshow
				// windows use the custom background. All other windows stay native.
				window_paint_hook.create(game::game_offset(0x102D1A00), window_paint_stub);
				console::info("[menu - open] installed: native openmenu hook with disk-source diagnostics\n");
				register_commands();
			}

			void pre_destroy() override
			{
				stock_guide_hook.clear();
				localized_message_hook.clear();
				open_menu_hook.clear();
				paint_menus_hook.clear();
				frontend_screen_hook.clear();
				window_paint_hook.clear();
				loaded_overlays.clear();
				loaded_menus.clear();
				loaded_menu_sources.clear();
				png_backgrounds.clear();
				preview_lists.clear();
				frontend_was_away = false;
				frontend_transition = false;
				frontend_return_requested = false;
				frontend_match_active = false;
				local_loadout_active = false;
				pending_scaleform_screen.clear();
				return_frontend_page.clear();
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
		loaded_overlays.clear();
		loaded_menu_sources.clear();
		png_backgrounds.clear();
		preview_lists.clear();
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
