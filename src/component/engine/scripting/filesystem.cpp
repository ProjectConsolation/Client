#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "filesystem.hpp"

#include "game/game.hpp"

#include <utils/io.hpp>
#include <utils/flags.hpp>
#include <utils/hook.hpp>
#include <utils/nt.hpp>
#include <ShlObj.h>

namespace filesystem
{
	namespace
	{
		utils::hook::detour fs_startup_hook;
		utils::hook::detour exec_hook;
		std::string game_directory;

		// Partial KisakCOD searchpath_s layout, verified against QoS PC
		// FS_AddIwdFilesForGameDirectory (0x10271F30). QoS has no checksum fields.
		struct search_path_view
		{
			search_path_view* next;
			unsigned char* iwd;
			void* directory;
			int localized, language;
		};
		static_assert(sizeof(search_path_view) == 20);
		static_assert(offsetof(search_path_view, iwd) == 4);

		bool consolation_iwd(const unsigned char* iwd)
		{
			// FS_LoadZipFile initializes three 256-byte strings: full filename,
			// basename, gamename. The mounting helper writes gamename at +512.
			return iwd && _strnicmp(reinterpret_cast<const char*>(iwd + 512), "consolation", 12) == 0;
		}

		void mount_iwds()
		{
			auto* search = *reinterpret_cast<search_path_view**>(game::game_offset(0x11A76550));
			if (!search) return; // Native filesystem has not started yet.
			for (auto* entry = search; entry; entry = entry->next)
				if (consolation_iwd(entry->iwd)) return; // fs_game already mounted this folder.
			// Mount only archives, not FS_AddGameDirectory: the latter changes
			// fs_gamedir and would redirect native writes/config ownership.
			reinterpret_cast<void(__cdecl*)(const char*, const char*)>(game::game_offset(0x10271F30))(
				game_directory.c_str(), "consolation");
			for (auto* entry = *reinterpret_cast<search_path_view**>(game::game_offset(0x11A76550));
				entry; entry = entry->next)
				if (consolation_iwd(entry->iwd))
					game::Com_Printf(10, "[FS] Mounted consolation IWD: %.255s\n", entry->iwd);
		}

		void close_file(const int handle)
		{
			// QoS FS_FCloseFile takes its handle in EAX, not on the stack.
			const auto target = game::game_offset(0x10270920);
			__asm
			{
				mov eax, handle
				call target
			}
		}

		struct file_guard
		{
			int handle{};
			file_guard() = default;
			file_guard(const file_guard&) = delete;
			file_guard& operator=(const file_guard&) = delete;
			~file_guard() { if (handle) close_file(handle); }
		};

		bool read_iwd_image_internal(const std::string& filename, std::vector<unsigned char>& data, std::string& source)
		{
			if (!*reinterpret_cast<void**>(game::game_offset(0x11A76550))) return false;
			const auto handles = game::game_offset(0x114E7390);
			// FS_FileHandleForThread uses 1..49 (main), 50..62 (stream/backend),
			// and 63 (DB). Avoid its fatal exhaustion path without borrowing an
			// occupied handle. Native image upload normally runs on the DB thread.
			const auto vacant = [handles](const int first, const int last)
			{
				for (int index = first; index <= last; ++index)
					if (!*reinterpret_cast<void**>(handles + 284 * index)) return true;
				return false;
			};
			if (!vacant(1, 49) || !vacant(50, 62) || !vacant(63, 63)) return false;
			file_guard file;
			const auto size = reinterpret_cast<int(__cdecl*)(const char*, int*)>(game::game_offset(0x10271D60))(
				filename.c_str(), &file.handle);
			if (!file.handle) return false;
			const auto* iwd = *reinterpret_cast<unsigned char**>(handles + 284 * file.handle + 20);
			if (!consolation_iwd(iwd)) return false; // Do not replace from stock main/devraw files.
			source = std::string(reinterpret_cast<const char*>(iwd), strnlen(reinterpret_cast<const char*>(iwd), 256))
				+ "::" + filename;
			if (size < 8 || static_cast<std::size_t>(size) > (64u * 1024u * 1024u))
				throw std::runtime_error("invalid IWD image size");
			data.resize(static_cast<std::size_t>(size));
			const auto read = reinterpret_cast<int(__cdecl*)(void*, int, int)>(game::game_offset(0x10270840))(
				data.data(), size, file.handle);
			if (read != size) throw std::runtime_error("could not read complete IWD image");
			return true;
		}
		bool initialized = false;
		bool engine_paths_enabled = false;
		bool engine_paths_registered = false;

		std::deque<std::filesystem::path>& get_search_paths_internal()
		{
			static std::deque<std::filesystem::path> search_paths{};
			return search_paths;
		}

		std::string get_default_players_directory()
		{
			char roaming_path[MAX_PATH]{};
			if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, roaming_path)))
			{
				return std::string(roaming_path) + "\\Activision\\Quantum of Solace\\players";
			}

			return {};
		}

		void add_engine_search_path(const char* folder_name, const char* root_path)
		{
			const auto func_loc = game::game_offset(0x10272AE0);

			__asm
			{
				push root_path
				mov edi, folder_name
				call func_loc
				add esp, 4
			}
		}

		void register_engine_search_paths()
		{
			if (!engine_paths_enabled || engine_paths_registered)
			{
				return;
			}

			static auto current_path = std::filesystem::current_path().string();
			if (current_path.empty())
			{
				return;
			}

			add_engine_search_path("consolation", current_path.c_str());
			// Legacy UI menus are resolved relative to the engine search path.
			// This maps ui_mp/serverbrowser.menu to consolation/menu/ui_mp/serverbrowser.menu.
			const auto menu_path = (std::filesystem::path(current_path) / "consolation" / "menu").string();
			add_engine_search_path("menu", menu_path.c_str());
			add_engine_search_path("raw", current_path.c_str());
			add_engine_search_path("userraw", current_path.c_str());
			engine_paths_registered = true;
		}

		int fs_startup_stub()
		{
			console::debug("[FS] Startup\n");

			initialized = true;
			engine_paths_registered = false;

			filesystem::register_path(L".");
			filesystem::register_path(L"consolation");
			filesystem::register_path(L"consolation/menu");
			filesystem::register_path(L"raw");
			filesystem::register_path(L"userraw");
			if (const auto players_directory = get_default_players_directory(); !players_directory.empty())
			{
				filesystem::register_path(players_directory);
			}
			//filesystem::register_path(L"devraw_shared");
			//filesystem::register_path(L"devraw");
			//filesystem::register_path(L"raw_shared");
			//filesystem::register_path(L"raw");
			//filesystem::register_path(L"main");

			// QoS PC FS_Startup takes no arguments (unlike KisakCOD).
			const auto result = fs_startup_hook.invoke<int>();
			register_engine_search_paths();
			mount_iwds();
			return result;
		}

		std::vector<std::filesystem::path> get_paths(const std::filesystem::path& path)
		{
			std::vector<std::filesystem::path> paths{};
			paths.push_back(path);
			return paths;
		}

		bool can_insert_path(const std::filesystem::path& path)
		{
			for (const auto& path_ : get_search_paths_internal())
			{
				if (path_ == path)
				{
					return false;
				}
			}

			return true;
		}

		const char* sys_default_install_path_stub()
		{
			static const auto host_path = utils::nt::get_host_module().get_folder();
			return host_path.c_str();
		}

		std::string normalize_exec_path(std::string path)
		{
			if (path.empty())
			{
				return path;
			}

			const auto slash = path.find_last_of("/\\");
			const auto dot = path.find_last_of('.');
			if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
			{
				path.append(".cfg");
			}

			return path;
		}

		void __cdecl exec_disk_log_stub(const int channel, const char* fmt, const char* requested_path)
		{
			(void)fmt;

			std::string real_path{};
			if (requested_path && requested_path[0] && filesystem::find_file(requested_path, &real_path))
			{
				game::Com_Printf(channel, "execing %s from %s\n", requested_path, real_path.c_str());
				return;
			}

			game::Com_Printf(channel, "execing %s from disk\n", requested_path ? requested_path : "<null>");
		}

		char exec_stub()
		{
			const auto nesting = *game::command_id;
			if (game::cmd_argc[nesting] != 2)
			{
				return exec_hook.invoke<char>();
			}

			const auto* const requested_path = game::cmd_argv[nesting][1];
			if (requested_path == nullptr || requested_path[0] == '\0')
			{
				return exec_hook.invoke<char>();
			}

			auto normalized_path = normalize_exec_path(requested_path);
			if (_stricmp(normalized_path.c_str(), "config_mp.cfg") == 0
				|| _stricmp(normalized_path.c_str(), "gfxConfig.cfg") == 0)
			{
				return exec_hook.invoke<char>();
			}

			std::string script_data{};
			std::string real_path{};
			if (!filesystem::read_file(normalized_path, &script_data, &real_path))
			{
				return exec_hook.invoke<char>();
			}

			game::Com_Printf(16, "execing %s from %s\n", normalized_path.c_str(), real_path.c_str());

			if (script_data.empty() || script_data.back() != '\n')
			{
				script_data.push_back('\n');
			}

			game::Cbuf_AddText(0, script_data.c_str());
			return 1;
		}
	}

	bool read_iwd_image(const std::string& path, std::vector<unsigned char>& data, std::string& source)
	{
		if (!path.starts_with("images/") || path.size() >= 256) return false;
		return read_iwd_image_internal(path, data, source);
	}

	std::string read_file(const std::string& path)
	{
		for (const auto& search_path : get_search_paths_internal())
		{
			const auto path_ = search_path / path;
			if (utils::io::file_exists(path_.generic_string()))
			{
				return utils::io::read_file(path_.generic_string());
			}
		}

		return {};
	}

	bool read_file(const std::string& path, std::string* data, std::string* real_path)
	{
		for (const auto& search_path : get_search_paths_internal())
		{
			const auto path_ = search_path / path;
			if (utils::io::read_file(path_.generic_string(), data))
			{
				if (real_path != nullptr)
				{
					*real_path = path_.generic_string();
				}

				return true;
			}
		}

		return false;
	}

	bool find_file(const std::string& path, std::string* real_path)
	{
		for (const auto& search_path : get_search_paths_internal())
		{
			const auto path_ = search_path / path;
			if (utils::io::file_exists(path_.generic_string()))
			{
				*real_path = path_.generic_string();
				return true;
			}
		}

		return false;
	}

	bool exists(const std::string& path)
	{
		for (const auto& search_path : get_search_paths_internal())
		{
			const auto path_ = search_path / path;
			if (utils::io::file_exists(path_.generic_string()))
			{
				return true;
			}
		}

		return false;
	}

	void enable_engine_search_paths(const bool enable)
	{
		engine_paths_enabled = enable;

		if (enable)
		{
			register_engine_search_paths();
		}
	}

	bool engine_search_paths_enabled()
	{
		return engine_paths_enabled;
	}

	void register_path(const std::filesystem::path& path)
	{
		if (!initialized)
		{
			return;
		}

		const auto paths = get_paths(path);
		for (const auto& path_ : paths)
		{
			if (can_insert_path(path_))
			{
				console::debug("[FS] Registering path '%s'\n", path_.generic_string().data());
				get_search_paths_internal().push_front(path_);
			}
		}
	}

	void unregister_path(const std::filesystem::path& path)
	{
		if (!initialized)
		{
			return;
		}

		const auto paths = get_paths(path);
		for (const auto& path_ : paths)
		{
			auto& search_paths = get_search_paths_internal();
			for (auto i = search_paths.begin(); i != search_paths.end();)
			{
				if (*i == path_)
				{
					console::debug("[FS] Unregistering path '%s'\n", path_.generic_string().data());
					i = search_paths.erase(i);
				}
				else
				{
					++i;
				}
			}
		}
	}

	std::vector<std::string> get_search_paths()
	{
		std::vector<std::string> paths{};

		for (const auto& path : get_search_paths_internal())
		{
			paths.push_back(path.generic_string());
		}

		return paths;
	}

	std::vector<std::string> get_search_paths_rev()
	{
		std::vector<std::string> paths{};
		const auto& search_paths = get_search_paths_internal();

		for (auto i = search_paths.rbegin(); i != search_paths.rend(); ++i)
		{
			paths.push_back(i->generic_string());
		}

		return paths;
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			game_directory = utils::nt::get_host_module().get_folder();
			fs_startup_hook.create(game::game_offset(0x10272D80), fs_startup_stub);
			mount_iwds();
			exec_hook.create(game::game_offset(0x103F5960), exec_stub);

			utils::hook::jump(game::game_offset(0x10274AA0), sys_default_install_path_stub);
			utils::hook::call(game::game_offset(0x103F589B), exec_disk_log_stub);
		}
	};
}

REGISTER_COMPONENT(filesystem::component)
