#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <ShlObj.h>
#include <unordered_map>
#include <unordered_set>

namespace profile_patches
{
	namespace
	{
		const char default_profile_config_name[] = "config_mp.cfg";
		const char custom_profile_config_name[] = "consolation_mp.cfg";
		const char decrypted_profile_config_name[] = "consolation_mp_decrypted.cfg";
		constexpr int active_profile_index = 0;
		constexpr auto live_clan_target_primary = 0x111CF170;
		constexpr auto live_clan_target_secondary = 0x111F1C00;
		constexpr auto clan_dirty_flags = 0x1149E6BC;
		void print_profile_message(const char* fmt, ...);
		std::string build_profile_file_path(const char* filename);

		const char* get_dvar_string(const std::size_t dvar_ptr_address)
		{
			const auto dvar = *reinterpret_cast<game::dvar_s**>(game::game_offset(dvar_ptr_address));
			if (!dvar || !dvar->current.string || !dvar->current.string[0])
			{
				return nullptr;
			}

			return dvar->current.string;
		}

		const char* get_dvar_string(game::dvar_s* dvar)
		{
			return dvar && dvar->current.string ? dvar->current.string : "";
		}

		std::string join_path(const std::string& base, const std::string& leaf)
		{
			if (base.empty())
			{
				return leaf;
			}

			if (base.back() == '\\' || base.back() == '/')
			{
				return base + leaf;
			}

			return base + "\\" + leaf;
		}

		std::string get_players_directory()
		{
			const auto path = get_dvar_string(0x114E7068);
			if (path && path[0])
			{
				return join_path(path, "players");
			}

			char roaming_path[MAX_PATH]{};
			if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, roaming_path)))
			{
				return join_path(join_path(roaming_path, "Activision\\Quantum of Solace"), "players");
			}

			if (!path)
			{
				return {};
			}

			return join_path(path, "players");
		}

		std::string get_profile_name()
		{
			const auto profile = get_dvar_string(0x10806824);
			return profile ? profile : "";
		}

		std::string build_players_file_path(const char* filename)
		{
			return join_path(get_players_directory(), filename);
		}

		std::string build_legacy_shared_config_path()
		{
			return build_players_file_path(custom_profile_config_name);
		}

		bool should_use_custom_profile_config()
		{
			return utils::io::file_exists(build_profile_file_path(custom_profile_config_name));
		}

		std::string build_runtime_config_path()
		{
			return build_profile_file_path(custom_profile_config_name);
		}

		std::string build_decrypted_output_path()
		{
			const auto path = build_profile_file_path(decrypted_profile_config_name);
			if (!path.empty())
			{
				return path;
			}

			return build_players_file_path(decrypted_profile_config_name);
		}

		std::string build_profile_file_path(const char* filename)
		{
			const auto players_directory = get_players_directory();
			const auto profile_name = get_profile_name();
			if (players_directory.empty() || profile_name.empty())
			{
				return {};
			}

			return join_path(join_path(players_directory, profile_name), filename);
		}

		std::string build_profile_config_path()
		{
			return build_profile_file_path("config_mp.cfg");
		}

		std::string build_custom_profile_config_path()
		{
			return build_profile_file_path(custom_profile_config_name);
		}

		bool looks_like_plaintext_config(const std::string& data)
		{
			if (data.empty())
			{
				return false;
			}

			std::size_t printable = 0;
			for (const auto ch : data)
			{
				const auto c = static_cast<unsigned char>(ch);
				if (c == '\r' || c == '\n' || c == '\t' || (c >= 32 && c < 127))
				{
					++printable;
				}
			}

			if ((printable * 100 / data.size()) < 85)
			{
				return false;
			}

			return data.find("seta ") != std::string::npos
				|| data.find("set ") != std::string::npos
				|| data.find("unbindall") != std::string::npos;
		}

		std::string to_lower(std::string value)
		{
			for (auto& ch : value)
			{
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}

			return value;
		}

		std::string trim_copy(const std::string& value)
		{
			const auto first = value.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
			{
				return {};
			}

			const auto last = value.find_last_not_of(" \t\r\n");
			return value.substr(first, last - first + 1);
		}

		std::string normalize_clan_name(const std::string& raw_value)
		{
			std::string result{};
			result.reserve(4);

			for (const auto ch : raw_value)
			{
				if (result.size() >= 4)
				{
					break;
				}

				const auto normalized = static_cast<unsigned char>(ch);
				if (normalized < 32
					|| normalized == '^'
					|| normalized == 0xA4
					|| normalized == '{'
					|| normalized == '}'
					|| normalized == '@')
				{
					continue;
				}

				result.push_back(static_cast<char>(normalized));
			}

			return trim_copy(result);
		}

		bool is_developer_reserved_clan_tag(const std::string& clan_name)
		{
			static const std::unordered_set<std::string> reserved_tags =
			{
				"csl",
			};

			return reserved_tags.find(to_lower(clan_name)) != reserved_tags.end();
		}

		void update_clan_name_state(const std::string& clan_name)
		{
			game::Dvar_SetString("clanName", clan_name.c_str());
			game::GamerProfile_UpdateProfileFromDvars(active_profile_index, 1);
			game::Live_UpdateClan(game::game_offset(live_clan_target_primary), clan_name.c_str());
			game::Live_UpdateClan(game::game_offset(live_clan_target_secondary), clan_name.c_str());
			*reinterpret_cast<std::uint32_t*>(game::game_offset(clan_dirty_flags)) |= 2u;
		}

		const char* find_canonical_dvar_name(const std::string& key)
		{
			static const std::unordered_map<std::string, const char*> canonical_names =
			{
				{"con_outputheightscale", "con_outputHeightScale"},
				{"cg_fovscale", "cg_fovScale"},
				{"input_viewsensitivity", "input_viewSensitivity"},
				{"ui_smallfont", "ui_smallFont"},
				{"ui_bigfont", "ui_bigFont"},
				{"ui_extrabigfont", "ui_extraBigFont"},
				{"r_lodscale", "r_lodScale"},
				{"m_rawinput", "m_rawInput"},
				{"bot_maxhealth", "bot_maxHealth"},
				{"g_debugvelocity", "g_debugVelocity"},
				{"g_debuglocalization", "g_debugLocalization"},
			};

			const auto canonical = canonical_names.find(to_lower(key));
			return canonical != canonical_names.end() ? canonical->second : nullptr;
		}

		bool try_parse_dvar_assignment(std::string& line, std::string* key)
		{
			const auto trimmed = trim_copy(line);
			if (trimmed.empty())
			{
				return false;
			}

			std::size_t offset = 0;
			if (trimmed.rfind("seta ", 0) == 0)
			{
				offset = 5;
			}
			else if (trimmed.rfind("set ", 0) == 0)
			{
				offset = 4;
			}
			else
			{
				return false;
			}

			const auto key_end = trimmed.find_first_of(" \t", offset);
			if (key_end == std::string::npos || key_end <= offset)
			{
				return false;
			}

			const auto raw_key = trimmed.substr(offset, key_end - offset);
			*key = to_lower(raw_key);
			if (const auto* canonical_name = find_canonical_dvar_name(raw_key))
			{
				line = trimmed.substr(0, offset) + canonical_name + trimmed.substr(key_end);
			}
			else
			{
				line = trimmed;
			}

			return !key->empty();
		}

		bool should_strip_runtime_only_dvar(const std::string& key)
		{
			return key == "r_aspectratiocustom"
				|| key == "r_aspectratiocustomenable"
				|| key == "r_custommode"
				|| key == "r_ultrawidecustommode";
		}

		std::string dedupe_config_text(const std::string& data)
		{
			const auto line_ending = data.find("\r\n") != std::string::npos ? "\r\n" : "\n";
			std::vector<std::string> lines{};
			std::vector<std::string> line_keys{};
			std::size_t start = 0;

			while (start <= data.size())
			{
				const auto end = data.find('\n', start);
				auto line = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (!line.empty() && line.back() == '\r')
				{
					line.pop_back();
				}

				lines.push_back(std::move(line));
				line_keys.emplace_back();

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}

			std::unordered_map<std::string, std::size_t> last_assignment_index{};
			for (std::size_t i = 0; i < lines.size(); ++i)
			{
				std::string key{};
				auto normalized_line = lines[i];
				if (!try_parse_dvar_assignment(normalized_line, &key))
				{
					continue;
				}

				if (should_strip_runtime_only_dvar(key))
				{
					continue;
				}

				lines[i] = std::move(normalized_line);
				line_keys[i] = key;
				last_assignment_index[key] = i;
			}

			std::string output{};
			for (std::size_t i = 0; i < lines.size(); ++i)
			{
				const auto& key = line_keys[i];
				if (!key.empty())
				{
					const auto last = last_assignment_index.find(key);
					if (last != last_assignment_index.end() && last->second != i)
					{
						continue;
					}
				}

				output.append(lines[i]);
				output.append(line_ending);
			}

			return output;
		}

		void dedupe_custom_profile_config()
		{
			if (!should_use_custom_profile_config())
			{
				return;
			}

			const auto path = build_custom_profile_config_path();
			std::string data{};
			if (!utils::io::read_file(path, &data) || !looks_like_plaintext_config(data))
			{
				return;
			}

			const auto deduped = dedupe_config_text(data);
			if (deduped == data)
			{
				return;
			}

			utils::io::write_file(path, deduped, false);
		}

		void decrypt_in_place(std::string& data)
		{
			using transform_t = int(__stdcall*)(int, int);
			const auto transform = reinterpret_cast<transform_t>(game::game_offset(0x103ECCA0));
			transform(reinterpret_cast<int>(data.data()), static_cast<int>(data.size()));
		}

		void print_profile_message(const char* fmt, ...)
		{
			char buffer[1024]{};
			va_list ap;
			va_start(ap, fmt);
			vsnprintf(buffer, sizeof(buffer), fmt, ap);
			va_end(ap);

			game::Com_Printf(16, "%s", buffer);
		}

		void print_runtime_config_path()
		{
			const auto runtime_path = build_runtime_config_path();
			if (runtime_path.empty())
			{
				print_profile_message("^3profile:config runtime config path is unavailable\n");
				return;
			}

			print_profile_message("^3profile:config %s\n", runtime_path.c_str());
		}

		bool convert_config_to_path(const std::string& source_path, const std::string& fallback_path, const std::string& output_path, const char* command_name)
		{
			if (output_path.empty())
			{
				print_profile_message("%s: players path is unavailable\n", command_name);
				return false;
			}

			print_profile_message("%s: output -> %s\n", command_name, output_path.c_str());
			if (!source_path.empty())
			{
				print_profile_message("%s: source -> %s\n", command_name, source_path.c_str());
			}

			std::string data;
			std::string loaded_from_path;

			if (!source_path.empty() && utils::io::read_file(source_path, &data))
			{
				loaded_from_path = source_path;
			}
			else if (!fallback_path.empty() && utils::io::read_file(fallback_path, &data))
			{
				loaded_from_path = fallback_path;
			}
			else
			{
				print_profile_message("%s: no config file found to migrate\n", command_name);
				return false;
			}

			if (looks_like_plaintext_config(data))
			{
				print_profile_message("%s: source already looks plaintext\n", command_name);
			}
			else
			{
				decrypt_in_place(data);
				if (!looks_like_plaintext_config(data))
				{
				print_profile_message("%s: decrypted data still looks invalid\n", command_name);
					return false;
				}

				print_profile_message("%s: decrypted %s\n", command_name, loaded_from_path.c_str());
			}

			if (!utils::io::write_file(output_path, data, false))
			{
				print_profile_message("%s: failed to write %s\n", command_name, output_path.c_str());
				return false;
			}

			print_profile_message("%s: wrote plaintext config to %s\n", command_name, output_path.c_str());
			return true;
		}

		void migrate_config()
		{
			convert_config_to_path(build_profile_config_path(), build_runtime_config_path(), build_decrypted_output_path(), "profile_decrypt_config");
		}

		void convert_config()
		{
			convert_config_to_path(
				build_legacy_shared_config_path(),
				build_profile_config_path(),
				build_custom_profile_config_path(),
				"profile_convert_config"
			);
		}

		bool try_decode_config_to_plaintext(std::string& data)
		{
			if (looks_like_plaintext_config(data))
			{
				return true;
			}

			decrypt_in_place(data);
			return looks_like_plaintext_config(data);
		}

		std::string get_custom_profile_import_path()
		{
			const auto profile_custom_path = build_custom_profile_config_path();
			if (!profile_custom_path.empty() && utils::io::file_exists(profile_custom_path))
			{
				return profile_custom_path;
			}

			const auto legacy_path = build_legacy_shared_config_path();
			if (!legacy_path.empty() && utils::io::file_exists(legacy_path))
			{
				return legacy_path;
			}

			return {};
		}

		bool import_custom_profile_config_into_stock()
		{
			const auto source_path = get_custom_profile_import_path();
			const auto stock_path = build_profile_config_path();
			const auto custom_path = build_custom_profile_config_path();
			if (source_path.empty() || stock_path.empty() || custom_path.empty())
			{
				return false;
			}

			std::string data{};
			if (!utils::io::read_file(source_path, &data) || !try_decode_config_to_plaintext(data))
			{
				return false;
			}

			const auto deduped = dedupe_config_text(data);
			if (source_path != custom_path || deduped != data)
			{
				utils::io::write_file(custom_path, deduped, false);
			}

			auto encrypted = deduped;
			decrypt_in_place(encrypted);
			return utils::io::write_file(stock_path, encrypted, false);
		}

		bool export_stock_profile_config_to_custom()
		{
			const auto stock_path = build_profile_config_path();
			const auto custom_path = build_custom_profile_config_path();
			if (stock_path.empty() || custom_path.empty())
			{
				return false;
			}

			std::string data{};
			if (!utils::io::read_file(stock_path, &data) || !try_decode_config_to_plaintext(data))
			{
				return false;
			}

			return utils::io::write_file(custom_path, dedupe_config_text(data), false);
		}

	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// Keep the profile helper commands available, but avoid touching the
			// stock signed-in config on the profile-login hot path.

			scheduler::once([]()
			{
				dedupe_custom_profile_config();

				command::add("profile_decrypt_config", []()
				{
					migrate_config();
				});

				command::add("clanName", [](const command::params& args)
				{
					auto* const clan_name = game::Dvar_FindVar("clanName");
					if (!clan_name)
					{
						print_profile_message("^1clanName: dvar is unavailable\n");
						return;
					}

					if (args.size() < 2)
					{
						print_profile_message("^3clanName: current tag is \"%s\"\n", get_dvar_string(clan_name));
						print_profile_message("^3usage: clanName <tag>\n");
						return;
					}

					const auto normalized = normalize_clan_name(args.join(1));
					if (normalized.empty())
					{
						print_profile_message("^1clanName: tag is empty or contains unsupported characters\n");
						return;
					}

#ifndef DEBUG
					if (is_developer_reserved_clan_tag(normalized))
					{
						print_profile_message("^1clanName: tag \"%s\" is reserved for developer builds\n", normalized.c_str());
						return;
					}
#endif

					update_clan_name_state(normalized);
					print_profile_message("^3clanName: set tag to \"%s\"\n", normalized.c_str());
				});

				command::add("profile_convert_config", []()
				{
					convert_config();
					dedupe_custom_profile_config();
					import_custom_profile_config_into_stock();
					print_runtime_config_path();
				});

				command::add("profile_sync_config", []()
				{
					const auto source_path = get_custom_profile_import_path();
					if (import_custom_profile_config_into_stock())
					{
						print_profile_message("^3profile:config synced %s into %s\n",
							source_path.c_str(), build_profile_config_path().c_str());
					}
					else
					{
						print_profile_message("^1profile:config failed to sync plaintext config into stock profile config\n");
					}
				});

				command::add("profile_show_config_path", []()
				{
					print_runtime_config_path();
				});

				command::add("profile_dedupe_config", []()
				{
					dedupe_custom_profile_config();
					print_runtime_config_path();
				});
			}, scheduler::main);

			scheduler::on_shutdown([]()
			{
				export_stock_profile_config_to_custom();
				dedupe_custom_profile_config();
			});
		}
	};
}

REGISTER_COMPONENT(profile_patches::component)
