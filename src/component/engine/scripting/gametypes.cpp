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

#include <algorithm>
#include <unordered_map>

namespace gametypes
{
	namespace
	{
		utils::hook::detour db_find_xasset_header_internal_hook;

		constexpr auto GAMETYPES_LIST = "maps/mp/gametypes/_gametypes.txt";
		constexpr auto GAMETYPE_PREFIX = "maps/mp/gametypes/";
		constexpr auto GAMETYPE_ENTRY_SIZE = 0x2C;
		constexpr auto UI_GAMETYPE_MAX = 0x20;

		std::unordered_map<std::string, game::RawFile*> loaded_gametype_rawfiles;
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

		bool is_gametype_rawfile(const game::XAssetType type, const char* name)
		{
			const auto normalized = normalize_gametype_path(name);
			return type == game::ASSET_TYPE_RAWFILE
				&& !normalized.empty()
				&& _strnicmp(normalized.c_str(), GAMETYPE_PREFIX, std::strlen(GAMETYPE_PREFIX)) == 0;
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
			const auto gametype_ids = parse_gametype_ids(gametype_list);
			if (gametype_ids.empty())
			{
				return;
			}

			auto* const count = reinterpret_cast<int*>(game::game_offset(0x113D1684));
			auto* const entries = reinterpret_cast<char*>(game::game_offset(0x113D1688));
			std::memset(entries, 0, GAMETYPE_ENTRY_SIZE * UI_GAMETYPE_MAX);

			auto written_count = 0;
			for (const auto& gametype_id : gametype_ids)
			{
				const auto display_file = std::format("{}{}.txt", GAMETYPE_PREFIX, gametype_id);
				const auto display_data = read_gametype_rawfile(display_file);
				if (display_data.empty())
				{
					continue;
				}

				const auto display_name = parse_gametype_display_name(display_data, gametype_id);
				auto* const entry = entries + (written_count * GAMETYPE_ENTRY_SIZE);

				copy_ui_gametype_string(entry, 12, gametype_id);
				copy_ui_gametype_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, display_name);
				++written_count;
			}

			*count = written_count;
			ui_gametype_list_refreshed = true;
			console::info("gametypes: refreshed UI gametype list with %i entries\n", written_count);
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
				const auto* const name = entry;
				const auto* const display = entry + 0x0C;
				output.append(std::format("{} \"{}\"\r\n", name, display));
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

				// Keep command registration in command.cpp if this dump helper is needed later.
			}

			void pre_destroy() override
			{
				db_find_xasset_header_internal_hook.clear();
				loaded_gametype_rawfiles.clear();
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
}

REGISTER_COMPONENT(gametypes::component)
