#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <ShlObj.h>

namespace profile_patches
{
	namespace
	{
		const char shared_profile_config_dir_format[] = "players/";
		const char default_profile_config_name[] = "config_mp.cfg";
		const char custom_profile_config_name[] = "consolation_mp.cfg";
		const char* get_dvar_string(const std::size_t dvar_ptr_address)
		{
			const auto dvar = *reinterpret_cast<game::dvar_s**>(game::game_offset(dvar_ptr_address));
			if (!dvar || !dvar->current.string || !dvar->current.string[0])
			{
				return nullptr;
			}

			return dvar->current.string;
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

		bool should_use_custom_profile_config()
		{
			return utils::io::file_exists(build_players_file_path(custom_profile_config_name));
		}

		const char* get_active_profile_config_name()
		{
			return should_use_custom_profile_config() ? custom_profile_config_name : default_profile_config_name;
		}

		std::string build_runtime_config_path()
		{
			return build_players_file_path(get_active_profile_config_name());
		}

		std::string build_decrypted_output_path()
		{
			return build_players_file_path("consolation_mp_decrypted.cfg");
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
			const auto output_path = build_players_file_path(custom_profile_config_name);
			convert_config_to_path(build_profile_config_path(), build_runtime_config_path(), output_path, "profile_convert_config");
		}

		void use_shared_profile_config_directory()
		{
			utils::hook::set<std::uint32_t>(
				game::game_offset(0x103F163B),
				static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(shared_profile_config_dir_format))
			);
		}

		void apply_active_profile_config_name()
		{
			const auto* active_name = get_active_profile_config_name();
			const auto active_name_value = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(active_name));

			utils::hook::set<std::uint32_t>(game::game_offset(0x103F18EF), active_name_value);
			utils::hook::set<std::uint32_t>(game::game_offset(0x103F1A53), active_name_value);
			utils::hook::set<std::uint32_t>(game::game_offset(0x103F7288), active_name_value);
		}

		void apply_profile_config_save_encryption()
		{
			if (should_use_custom_profile_config())
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F6D71), 0x83);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F6D72), 0xC4);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F6D73), 0x08);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F6D74), 0x90);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F6D75), 0x90);
				return;
			}

			utils::hook::set<std::uint8_t>(game::game_offset(0x103F6D71), 0xE8);
			utils::hook::set<std::int32_t>(
				game::game_offset(0x103F6D72),
				static_cast<std::int32_t>(game::game_offset(0x103ECD50) - (game::game_offset(0x103F6D71) + 5))
			);
		}

	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			use_shared_profile_config_directory();
			apply_active_profile_config_name();
			apply_profile_config_save_encryption();

			scheduler::loop([]()
			{
				apply_active_profile_config_name();
			}, scheduler::main, 1s);

			scheduler::once([]()
			{
				print_runtime_config_path();
			}, scheduler::main, 2s);

			command::add("profile_decrypt_config", []()
			{
				migrate_config();
			});

			command::add("profile_convert_config", []()
			{
				convert_config();
				apply_profile_config_save_encryption();
				print_runtime_config_path();
			});

			command::add("profile_show_config_path", []()
			{
				print_runtime_config_path();
			});
		}
	};
}

REGISTER_COMPONENT(profile_patches::component)
