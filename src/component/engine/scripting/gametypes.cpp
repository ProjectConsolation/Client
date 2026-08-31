#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "filesystem.hpp"
#include "gametypes.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/memory.hpp>
#include <utils/string.hpp>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace gametypes
{
	namespace
	{
		utils::hook::detour db_find_xasset_header_internal_hook;

		constexpr auto GAMETYPES_LIST = "maps/mp/gametypes/_gametypes.txt";
		constexpr auto GAMETYPE_PREFIX = "maps/mp/gametypes/";
		constexpr auto GAMETYPE_ENTRY_SIZE = 0x2C;
		constexpr auto UI_GAMETYPE_MAX = 0x20;
		constexpr auto UI_GAMETYPE_COUNT = 0x113D1684;
		constexpr auto UI_GAMETYPE_ENTRIES = 0x113D1688;
		constexpr auto UI_GAMETYPE_ALT_COUNT = 0x113D31EC;
		constexpr auto UI_GAMETYPE_ALT_ENTRIES = 0x113D31F0;

		std::unordered_map<std::string, game::RawFile*> loaded_gametype_rawfiles;
		std::unordered_map<std::string, game::RawFile*> loaded_scaleform_rawfiles;
		bool ui_gametype_list_refreshed = false;

		std::string normalize_gametype_path(const char* name)
		{
			std::string normalized = name ? name : "";
			for (auto& ch : normalized)
			{
				if (ch == '\\')
				{
					ch = '/';
				}
			}

			return normalized;
		}

		std::string trim(std::string value)
		{
			const auto first = value.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
			{
				return {};
			}

			const auto last = value.find_last_not_of(" \t\r\n");
			return value.substr(first, last - first + 1);
		}

		void copy_ui_gametype_string(char* dest, const std::size_t dest_size, const std::string& value)
		{
			if (dest == nullptr || dest_size == 0)
			{
				return;
			}

			strncpy_s(dest, dest_size, value.c_str(), _TRUNCATE);
		}

		std::string bounded_string(const char* value, const std::size_t max_length)
		{
			if (!value || max_length == 0)
			{
				return {};
			}

			std::string result{};
			result.reserve(max_length);
			for (std::size_t i = 0; i < max_length && value[i] != '\0'; ++i)
			{
				const auto ch = static_cast<unsigned char>(value[i]);
				result.push_back(ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.');
			}

			return result;
		}

		bool is_gametype_rawfile(const game::XAssetType type, const char* name)
		{
			const auto normalized = normalize_gametype_path(name);
			return type == game::ASSET_TYPE_RAWFILE
				&& !normalized.empty()
				&& _strnicmp(normalized.c_str(), GAMETYPE_PREFIX, std::strlen(GAMETYPE_PREFIX)) == 0;
		}

		bool is_scaleform_rawfile(const game::XAssetType type, const char* name)
		{
			const auto normalized = normalize_gametype_path(name);
			return type == game::ASSET_TYPE_RAWFILE
				&& normalized.starts_with("scaleform/")
				&& normalized.ends_with(".gfx");
		}

		bool rawfile_has_data(const game::RawFile* rawfile)
		{
			return rawfile != nullptr && rawfile->buffer != nullptr && rawfile->len > 0;
		}

		const char* get_zone_name(const unsigned char zone_index)
		{
			const auto* const zones = reinterpret_cast<game::XZone*>(game::game_offset(0x10AB8188));
			const auto* const zone = &zones[zone_index];
			return zone->name[0] ? zone->name : "";
		}

		bool is_patch_zone(const char* zone_name)
		{
			return zone_name != nullptr && _strnicmp(zone_name, "patch_", 6) == 0;
		}

		bool rawfile_name_equals(const game::RawFile* rawfile, const std::string& name)
		{
			if (!rawfile || !rawfile->name)
			{
				return false;
			}

			return _stricmp(normalize_gametype_path(rawfile->name).c_str(), name.c_str()) == 0;
		}

		game::RawFile* find_patch_gametype_rawfile(const std::string& name)
		{
			game::RawFile* fallback_patch_rawfile = nullptr;

			game::DB_EnumXAssetEntries(game::ASSET_TYPE_RAWFILE, [&](game::XAssetEntryPoolEntry* pool_entry)
			{
				if (pool_entry == nullptr)
				{
					return;
				}

				const auto& entry = pool_entry->entry;
				auto* const rawfile = entry.asset.header.rawfile;
				if (!rawfile_has_data(rawfile) || !rawfile_name_equals(rawfile, name))
				{
					return;
				}

				const auto* const zone_name = get_zone_name(static_cast<unsigned char>(entry.zoneIndex));
				if (!is_patch_zone(zone_name))
				{
					return;
				}

				// Prefer the project patch if multiple patch zones carry the same rawfile.
				if (_stricmp(zone_name, "patch_consolation") == 0)
				{
					fallback_patch_rawfile = rawfile;
					return;
				}

				if (!fallback_patch_rawfile)
				{
					fallback_patch_rawfile = rawfile;
				}
			}, true);

			return fallback_patch_rawfile;
		}

		game::RawFile* make_rawfile(const std::string& name, const std::string& data)
		{
			auto* rawfile = utils::memory::allocate<game::RawFile>();
			auto* rawfile_name = static_cast<char*>(utils::memory::allocate(name.size() + 1));
			auto* buffer = static_cast<char*>(utils::memory::allocate(data.size() + 1));

			std::memcpy(rawfile_name, name.data(), name.size());
			std::memcpy(buffer, data.data(), data.size());
			rawfile_name[name.size()] = '\0';
			buffer[data.size()] = '\0';

			rawfile->name = rawfile_name;
			rawfile->len = static_cast<unsigned int>(data.size() + 1);
			rawfile->buffer = buffer;
			return rawfile;
		}

		game::RawFile* make_binary_rawfile(const std::string& name, const std::string& data)
		{
			auto* rawfile = utils::memory::allocate<game::RawFile>();
			auto* rawfile_name = static_cast<char*>(utils::memory::allocate(name.size() + 1));
			auto* buffer = static_cast<char*>(utils::memory::allocate(data.size()));

			std::memcpy(rawfile_name, name.data(), name.size());
			std::memcpy(buffer, data.data(), data.size());
			rawfile_name[name.size()] = '\0';

			rawfile->name = rawfile_name;
			rawfile->len = static_cast<unsigned int>(data.size());
			rawfile->buffer = buffer;
			return rawfile;
		}

		game::RawFile* load_custom_gametype_rawfile(const char* name)
		{
			const auto normalized_name = normalize_gametype_path(name);
			if (const auto existing = loaded_gametype_rawfiles.find(normalized_name); existing != loaded_gametype_rawfiles.end())
			{
				return existing->second;
			}

			std::string data{};
			std::string real_path{};
			if (!filesystem::read_file(normalized_name, &data, &real_path))
			{
				return nullptr;
			}

			auto* rawfile = make_rawfile(normalized_name, data);
			loaded_gametype_rawfiles[normalized_name] = rawfile;

			console::info("gametypes: loaded raw fallback %s from %s\n", normalized_name.c_str(), real_path.c_str());
			return rawfile;
		}

		game::RawFile* find_patch_scaleform_rawfile(const std::string& name)
		{
			game::RawFile* fallback_patch_rawfile = nullptr;

			game::DB_EnumXAssetEntries(game::ASSET_TYPE_RAWFILE, [&](game::XAssetEntryPoolEntry* pool_entry)
			{
				if (pool_entry == nullptr)
				{
					return;
				}

				const auto& entry = pool_entry->entry;
				auto* const rawfile = entry.asset.header.rawfile;
				if (!rawfile_has_data(rawfile) || !rawfile_name_equals(rawfile, name))
				{
					return;
				}

				const auto* const zone_name = get_zone_name(static_cast<unsigned char>(entry.zoneIndex));
				if (!is_patch_zone(zone_name))
				{
					return;
				}

				if (_stricmp(zone_name, "patch_consolation") == 0)
				{
					fallback_patch_rawfile = rawfile;
					return;
				}

				if (!fallback_patch_rawfile)
				{
					fallback_patch_rawfile = rawfile;
				}
			}, true);

			return fallback_patch_rawfile;
		}

		game::RawFile* load_custom_scaleform_rawfile(const char* name)
		{
			const auto normalized_name = normalize_gametype_path(name);
			if (const auto existing = loaded_scaleform_rawfiles.find(normalized_name); existing != loaded_scaleform_rawfiles.end())
			{
				return existing->second;
			}

			const auto ui_mp_name = std::string("ui_mp/") + normalized_name;
			for (const auto& candidate_name : { ui_mp_name, normalized_name })
			{
				std::string data{};
				std::string real_path{};
				if (!filesystem::read_file(candidate_name, &data, &real_path))
				{
					continue;
				}

				auto* rawfile = make_binary_rawfile(normalized_name, data);
				loaded_scaleform_rawfiles[normalized_name] = rawfile;

				console::info("scaleform: loaded raw override %s from %s\n", normalized_name.c_str(), real_path.c_str());
				return rawfile;
			}

			return nullptr;
		}

		game::XAssetHeader db_find_xasset_header_internal_stub(const game::XAssetType type, const char* name, const int create_default)
		{
			if (is_gametype_rawfile(type, name))
			{
				const auto normalized_name = normalize_gametype_path(name);
				if (auto* patch_rawfile = find_patch_gametype_rawfile(normalized_name))
				{
					game::XAssetHeader header{};
					header.rawfile = patch_rawfile;
					return header;
				}

				if (auto* rawfile = load_custom_gametype_rawfile(name))
				{
					game::XAssetHeader header{};
					header.rawfile = rawfile;
					return header;
				}

				const auto fastfile_header = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, normalized_name.c_str(), create_default);
				if (rawfile_has_data(fastfile_header.rawfile))
				{
					return fastfile_header;
				}
			}

			if (is_scaleform_rawfile(type, name))
			{
				const auto normalized_name = normalize_gametype_path(name);
				if (auto* patch_rawfile = find_patch_scaleform_rawfile(normalized_name))
				{
					game::XAssetHeader header{};
					header.rawfile = patch_rawfile;
					return header;
				}

				if (auto* rawfile = load_custom_scaleform_rawfile(normalized_name.c_str()))
				{
					game::XAssetHeader header{};
					header.rawfile = rawfile;
					return header;
				}
			}

			return db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, name, create_default);
		}

		std::string get_rawfile_buffer(const game::RawFile* rawfile)
		{
			if (!rawfile || !rawfile->buffer)
			{
				return {};
			}

			if (rawfile->len > 0)
			{
				const auto size = rawfile->buffer[rawfile->len - 1] == '\0'
					? rawfile->len - 1
					: rawfile->len;
				return { rawfile->buffer, rawfile->buffer + size };
			}

			return rawfile->buffer;
		}

		std::string read_gametype_rawfile(const std::string& name)
		{
			const auto header = game::DB_FindXAssetHeader_Internal(game::ASSET_TYPE_RAWFILE, name.c_str(), 1);
			return get_rawfile_buffer(header.rawfile);
		}

		std::vector<std::string> parse_gametype_ids(const std::string& data)
		{
			std::vector<std::string> ids{};
			std::istringstream stream(data);
			std::string line{};

			while (std::getline(stream, line) && ids.size() < UI_GAMETYPE_MAX)
			{
				if (const auto comment = line.find("//"); comment != std::string::npos)
				{
					line.erase(comment);
				}

				line = trim(line);
				if (!line.empty())
				{
					ids.push_back(line);
				}
			}

			return ids;
		}

		bool is_hidden_gametype_id(const std::string& gametype_id)
		{
			return _stricmp(gametype_id.c_str(), "menu") == 0;
		}

		void append_unique_gametype_id(std::vector<std::string>& ids, std::unordered_set<std::string>& known_ids, const std::string& gametype_id)
		{
			auto normalized_id = trim(gametype_id);
			if (normalized_id.empty() || is_hidden_gametype_id(normalized_id) || ids.size() >= UI_GAMETYPE_MAX)
			{
				return;
			}

			std::transform(normalized_id.begin(), normalized_id.end(), normalized_id.begin(), [](const unsigned char ch)
			{
				return static_cast<char>(std::tolower(ch));
			});

			if (known_ids.insert(normalized_id).second)
			{
				ids.push_back(std::move(normalized_id));
			}
		}

		std::string get_gametype_id_from_display_path(const std::string& path)
		{
			auto normalized = normalize_gametype_path(path.c_str());
			if (_strnicmp(normalized.c_str(), GAMETYPE_PREFIX, std::strlen(GAMETYPE_PREFIX)) != 0)
			{
				const auto marker = normalized.find(GAMETYPE_PREFIX);
				if (marker == std::string::npos)
				{
					return {};
				}

				normalized = normalized.substr(marker);
			}

			if (!utils::string::ends_with(normalized, ".txt") || _stricmp(normalized.c_str(), GAMETYPES_LIST) == 0)
			{
				return {};
			}

			const auto slash = normalized.find_last_of('/');
			const auto dot = normalized.find_last_of('.');
			if (slash == std::string::npos || dot == std::string::npos || dot <= slash + 1)
			{
				return {};
			}

			return normalized.substr(slash + 1, dot - slash - 1);
		}

		void append_patch_gametype_ids(std::vector<std::string>& ids, std::unordered_set<std::string>& known_ids)
		{
			game::DB_EnumXAssetEntries(game::ASSET_TYPE_RAWFILE, [&](game::XAssetEntryPoolEntry* pool_entry)
			{
				if (ids.size() >= UI_GAMETYPE_MAX || pool_entry == nullptr)
				{
					return;
				}

				const auto& entry = pool_entry->entry;
				const auto* const rawfile = entry.asset.header.rawfile;
				if (!rawfile_has_data(rawfile) || !rawfile->name)
				{
					return;
				}

				const auto* const zone_name = get_zone_name(static_cast<unsigned char>(entry.zoneIndex));
				if (!is_patch_zone(zone_name))
				{
					return;
				}

				append_unique_gametype_id(ids, known_ids, get_gametype_id_from_display_path(rawfile->name));
			}, true);
		}

		void append_disk_gametype_ids(std::vector<std::string>& ids, std::unordered_set<std::string>& known_ids)
		{
			for (const auto& search_path : filesystem::get_search_paths())
			{
				if (ids.size() >= UI_GAMETYPE_MAX)
				{
					break;
				}

				const auto directory = std::filesystem::path(search_path) / "maps/mp/gametypes";
				if (!utils::io::directory_exists(directory.generic_string()))
				{
					continue;
				}

				for (const auto& file : utils::io::list_files(directory.generic_string()))
				{
					if (ids.size() >= UI_GAMETYPE_MAX)
					{
						break;
					}

					append_unique_gametype_id(ids, known_ids, get_gametype_id_from_display_path(file));
				}
			}
		}

		std::vector<std::string> get_visible_gametype_ids(const std::string& gametype_list)
		{
			std::vector<std::string> ids{};
			std::unordered_set<std::string> known_ids{};

			for (const auto& gametype_id : parse_gametype_ids(gametype_list))
			{
				append_unique_gametype_id(ids, known_ids, gametype_id);
			}

			append_patch_gametype_ids(ids, known_ids);
			append_disk_gametype_ids(ids, known_ids);
			return ids;
		}

		std::string parse_gametype_display_name(const std::string& data, const std::string& fallback)
		{
			auto text = trim(data);
			if (text.empty())
			{
				return fallback;
			}

			if (const auto comment = text.find("//"); comment != std::string::npos)
			{
				text = trim(text.substr(0, comment));
			}

			if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
			{
				text = text.substr(1, text.size() - 2);
			}

			return text.empty() ? fallback : text;
		}

		void rebuild_ui_gametype_cache()
		{
			const auto gametype_list = read_gametype_rawfile(GAMETYPES_LIST);
			if (gametype_list.empty())
			{
				console::warn("gametypes: cannot refresh UI list because %s was empty or unavailable\n", GAMETYPES_LIST);
				return;
			}

			const auto gametype_ids = get_visible_gametype_ids(gametype_list);
			if (gametype_ids.empty())
			{
				console::warn("gametypes: cannot refresh UI list because %s parsed no entries\n", GAMETYPES_LIST);
				return;
			}

			auto* const count = reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_COUNT));
			auto* const entries = reinterpret_cast<char*>(game::game_offset(UI_GAMETYPE_ENTRIES));
			auto* const alt_count = reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_ALT_COUNT));
			auto* const alt_entries = reinterpret_cast<char*>(game::game_offset(UI_GAMETYPE_ALT_ENTRIES));

			std::memset(entries, 0, GAMETYPE_ENTRY_SIZE * UI_GAMETYPE_MAX);
			std::memset(alt_entries, 0, GAMETYPE_ENTRY_SIZE * UI_GAMETYPE_MAX);

			auto written_count = 0;
			auto visible_count = 0;
			std::vector<std::string> hidden_gametype_ids{};
			for (const auto& gametype_id : gametype_ids)
			{
				const auto display_file = std::format("{}{}.txt", GAMETYPE_PREFIX, gametype_id);
				const auto display_data = read_gametype_rawfile(display_file);
				if (display_data.empty())
				{
					hidden_gametype_ids.push_back(gametype_id);
					continue;
				}

				const auto display_name = parse_gametype_display_name(display_data, gametype_id);
				auto* const entry = entries + (written_count * GAMETYPE_ENTRY_SIZE);
				auto* const alt_entry = alt_entries + (written_count * GAMETYPE_ENTRY_SIZE);

				copy_ui_gametype_string(entry, 12, gametype_id);
				copy_ui_gametype_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, display_name);
				copy_ui_gametype_string(alt_entry, 12, gametype_id);
				copy_ui_gametype_string(alt_entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, display_name);
				++written_count;
				++visible_count;
			}

			for (const auto& gametype_id : hidden_gametype_ids)
			{
				if (written_count >= UI_GAMETYPE_MAX)
				{
					break;
				}

				auto* const entry = entries + (written_count * GAMETYPE_ENTRY_SIZE);
				auto* const alt_entry = alt_entries + (written_count * GAMETYPE_ENTRY_SIZE);

				copy_ui_gametype_string(entry, 12, gametype_id);
				copy_ui_gametype_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, gametype_id);
				copy_ui_gametype_string(alt_entry, 12, gametype_id);
				copy_ui_gametype_string(alt_entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, gametype_id);
				++written_count;
			}

			*count = written_count;
			*alt_count = written_count;
			ui_gametype_list_refreshed = true;
			console::info("gametypes: refreshed UI gametype lists with %i visible entries, %i total entries\n", visible_count, written_count);
		}

		void dump_loaded_gametypes_rawfile()
		{
			std::string data{};
			std::string source{};
			if (!filesystem::read_file(GAMETYPES_LIST, &data, &source))
			{
				const auto asset = game::DB_FindXAssetHeader_Internal(game::ASSET_TYPE_RAWFILE, GAMETYPES_LIST, 1);
				data = get_rawfile_buffer(asset.rawfile);
				source = "fastfile";
			}

			if (data.empty())
			{
				console::warn("dumpGametypes: %s was empty or unavailable\n", GAMETYPES_LIST);
				return;
			}

			const auto out_path = "consolation/gametypes_dump/_gametypes.txt";
			if (utils::io::write_file(out_path, data))
			{
				console::info("dumpGametypes: dumped %s from %s to %s\n", GAMETYPES_LIST, source.c_str(), out_path);
			}
			else
			{
				console::error("dumpGametypes: failed to write %s\n", out_path);
			}
		}

		void dump_parsed_ui_gametypes()
		{
			const auto count = std::clamp(*reinterpret_cast<int*>(game::game_offset(0x113D1684)), 0, UI_GAMETYPE_MAX);
			const auto* const entries = reinterpret_cast<const char*>(game::game_offset(0x113D1688));

			std::string output{};
			for (auto i = 0; i < count; ++i)
			{
				const auto* const entry = entries + (i * GAMETYPE_ENTRY_SIZE);
				output.append(std::format(
					"{} \"{}\"\r\n",
					bounded_string(entry, 12),
					bounded_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C)));
			}

			const auto out_path = "consolation/gametypes_dump/parsed_ui_gametypes.txt";
			if (utils::io::write_file(out_path, output))
			{
				console::info("dumpGametypes: dumped %i parsed UI gametype(s) to %s\n", count, out_path);
			}
			else
			{
				console::error("dumpGametypes: failed to write %s\n", out_path);
			}
		}

		bool safe_read_byte(const std::uintptr_t address, unsigned char* value)
		{
			if (!value)
			{
				return false;
			}

			SIZE_T bytes_read = 0;
			return ReadProcessMemory(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(address),
				value,
				sizeof(*value),
				&bytes_read) && bytes_read == sizeof(*value);
		}

		std::string get_exe_directory()
		{
			char path[MAX_PATH]{};
			const auto length = GetModuleFileNameA(nullptr, path, sizeof(path));
			if (length == 0 || length >= sizeof(path))
			{
				return {};
			}

			std::string result = path;
			const auto separator = result.find_last_of("/\\");
			if (separator == std::string::npos)
			{
				return {};
			}

			result.resize(separator);
			return result;
		}

		std::string resolve_game_path(const std::string& path)
		{
			if (path.size() > 2 && path[1] == ':')
			{
				return path;
			}

			const auto base = get_exe_directory();
			if (base.empty())
			{
				return path;
			}

			return base + "\\" + path;
		}

		bool create_directory_tree(const std::string& path, DWORD* error)
		{
			if (path.empty())
			{
				return true;
			}

			auto create_one = [error](const std::string& directory)
			{
				if (directory.empty() || (directory.size() == 2 && directory[1] == ':'))
				{
					return true;
				}

				if (CreateDirectoryA(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
				{
					return true;
				}

				if (error)
				{
					*error = GetLastError();
				}

				return false;
			};

			for (std::size_t i = 0; i < path.size(); ++i)
			{
				if (path[i] != '/' && path[i] != '\\')
				{
					continue;
				}

				if (!create_one(path.substr(0, i)))
				{
					return false;
				}
			}

			return create_one(path);
		}

		bool write_file_noexcept(const std::string& path, const std::string& data, DWORD* error)
		{
			const auto resolved_path = resolve_game_path(path);
			const auto separator = resolved_path.find_last_of("/\\");
			if (separator != std::string::npos && !create_directory_tree(resolved_path.substr(0, separator), error))
			{
				return false;
			}

			const auto file = CreateFileA(resolved_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				if (error)
				{
					*error = GetLastError();
				}

				return false;
			}

			DWORD written = 0;
			const auto size = static_cast<DWORD>(std::min<std::size_t>(data.size(), MAXDWORD));
			const auto ok = WriteFile(file, data.data(), size, &written, nullptr) && written == size;
			if (!ok && error)
			{
				*error = GetLastError();
			}

			CloseHandle(file);
			return ok;
		}

		void append_bytes(std::string& output, const char* label, const std::uintptr_t address, const std::size_t length)
		{
			output.append(std::format("  {} @ {:08X}: ", label, address));
			for (std::size_t i = 0; i < length; ++i)
			{
				unsigned char byte = 0;
				if (safe_read_byte(address + i, &byte))
				{
					output.append(std::format("{:02X} ", byte));
				}
				else
				{
					output.append("?? ");
				}
			}

			output.append("\r\n");
		}

		void register_commands()
		{
			command::add("dumpGametypes", [](const command::params&)
			{
				dump_loaded_gametypes_rawfile();
				dump_parsed_ui_gametypes();
			});
		}

		class component final : public component_interface
		{
		public:
			void post_load() override
			{
				db_find_xasset_header_internal_hook.create(game::DB_FindXAssetHeader_Internal, db_find_xasset_header_internal_stub);

				// The frontend feeder count path subtracts one to hide the stock "menu" sentinel.
				// We rebuild the cache without that sentinel, so expose the full count.
				utils::hook::nop(game::game_offset(0x102DEA4A), 3);

				// The gametype feeder row resolver has its own hide-last subtraction before
				// returning row text/id. Keep it in sync with the count path above.
				utils::hook::nop(game::game_offset(0x102DE2F5), 3);

				// Scaleform requests a fixed-size row window. Ignore that window limit and let the
				// feeder submit every available item up to the real total count.
				utils::hook::nop(game::game_offset(0x102D4A12), 10);

				// Keep command registration in command.cpp if this dump helper is needed later.
			}

			void pre_destroy() override
			{
				db_find_xasset_header_internal_hook.clear();
				loaded_gametype_rawfiles.clear();
				loaded_scaleform_rawfiles.clear();
				ui_gametype_list_refreshed = false;
			}
		};
	}

	void refresh_ui_gametype_list()
	{
		if (ui_gametype_list_refreshed)
		{
			return;
		}

		rebuild_ui_gametype_cache();
	}

	void force_refresh_ui_gametype_list()
	{
		rebuild_ui_gametype_cache();
	}

	void dump_debug_state()
	{
		const auto count = std::clamp(*reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_COUNT)), 0, UI_GAMETYPE_MAX);
		const auto alt_count = std::clamp(*reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_ALT_COUNT)), 0, UI_GAMETYPE_MAX);
		const auto* const entries = reinterpret_cast<const char*>(game::game_offset(UI_GAMETYPE_ENTRIES));
		const auto* const alt_entries = reinterpret_cast<const char*>(game::game_offset(UI_GAMETYPE_ALT_ENTRIES));

		std::string output{};
		output.append(std::format("primary count: {} @ {:08X}\r\n", count, game::game_offset(UI_GAMETYPE_COUNT)));
		output.append(std::format("primary entries: {:08X}\r\n", game::game_offset(UI_GAMETYPE_ENTRIES)));
		for (auto i = 0; i < count; ++i)
		{
			const auto* const entry = entries + (i * GAMETYPE_ENTRY_SIZE);
			output.append(std::format(
				"  [{}] id='{}' display='{}'\r\n",
				i,
				bounded_string(entry, 12),
				bounded_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C)));
		}

		output.append(std::format("\r\nsecondary count: {} @ {:08X}\r\n", alt_count, game::game_offset(UI_GAMETYPE_ALT_COUNT)));
		output.append(std::format("secondary entries: {:08X}\r\n", game::game_offset(UI_GAMETYPE_ALT_ENTRIES)));
		for (auto i = 0; i < alt_count; ++i)
		{
			const auto* const entry = alt_entries + (i * GAMETYPE_ENTRY_SIZE);
			output.append(std::format(
				"  [{}] id='{}' display='{}'\r\n",
				i,
				bounded_string(entry, 12),
				bounded_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C)));
		}

		output.append("\r\nknown feeder patches:\r\n");
		output.append(std::format("  count hide-last instruction: {:08X}\r\n", game::game_offset(0x102DEA4A)));
		output.append(std::format("  gametype row resolver: {:08X}\r\n", game::game_offset(0x102DE2EF)));
		output.append(std::format("  gametype row hide-last instruction: {:08X}\r\n", game::game_offset(0x102DE2F5)));
		output.append(std::format("  feeder draw switch case 271: {:08X}\r\n", game::game_offset(0x102DC430)));
		append_bytes(output, "count hide-last bytes", game::game_offset(0x102DEA4A), 3);
		append_bytes(output, "row hide-last bytes", game::game_offset(0x102DE2F5), 3);
		append_bytes(output, "scaleform window-limit bytes", game::game_offset(0x102D4A12), 10);

		output.append("\r\nnote: ASSET_TYPE_MENU is the IW .menu layer, not Scaleform .gfx, so this dump intentionally avoids enumerating it.\r\n");

		const auto out_path = "consolation/gametypes_dump/debug_state.txt";
		DWORD error = ERROR_SUCCESS;
		if (write_file_noexcept(out_path, output, &error))
		{
			console::info("dumpGametypesDebug: wrote %s\n", out_path);
		}
		else
		{
			console::error("dumpGametypesDebug: failed to write %s (GetLastError=%lu)\n", out_path, error);
		}
	}
}

REGISTER_COMPONENT(gametypes::component)
