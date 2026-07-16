#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "filesystem.hpp"

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
		constexpr auto GAMETYPES_LIST = "maps/mp/gametypes/_gametypes.txt";
		constexpr auto GAMETYPE_PREFIX = "maps/mp/gametypes/";
		constexpr auto GAMETYPE_ENTRY_SIZE = 0x2C;
		constexpr auto UI_GAMETYPE_MAX = 0x20;

		std::unordered_map<std::string, game::RawFile*> loaded_gametype_rawfiles;

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

		bool rawfile_is_from_patch_zone(const game::RawFile* rawfile)
		{
			if (!rawfile)
			{
				return false;
			}

			auto found_patch_zone = false;
			game::DB_EnumXAssetEntries(game::ASSET_TYPE_RAWFILE, [&](game::XAssetEntryPoolEntry* pool_entry)
			{
				if (found_patch_zone || pool_entry == nullptr)
				{
					return;
				}

				const auto& entry = pool_entry->entry;
				if (entry.asset.header.rawfile == rawfile && is_patch_zone(get_zone_name(static_cast<unsigned char>(entry.zoneIndex))))
				{
					found_patch_zone = true;
				}
			}, true);

			return found_patch_zone;
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

		game::XAssetHeader DB_FindXAssetHeader_Internal_gametype_stub(const game::XAssetType type, const char* name, const int create_default)
		{
			if (is_gametype_rawfile(type, name))
			{
				const auto fastfile_header = game::DB_FindXAssetHeader_Internal(type, normalize_gametype_path(name).c_str(), create_default);
				if (rawfile_has_data(fastfile_header.rawfile) && rawfile_is_from_patch_zone(fastfile_header.rawfile))
				{
					return fastfile_header;
				}

				if (auto* rawfile = load_custom_gametype_rawfile(name))
				{
					game::XAssetHeader header{};
					header.rawfile = rawfile;
					return header;
				}

				if (rawfile_has_data(fastfile_header.rawfile))
				{
					return fastfile_header;
				}
			}

			return game::DB_FindXAssetHeader_Internal(type, name, create_default);
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
				// Script metadata gametype list.
				utils::hook::call(game::game_offset(0x101A253E), DB_FindXAssetHeader_Internal_gametype_stub);
				utils::hook::call(game::game_offset(0x101A25FB), DB_FindXAssetHeader_Internal_gametype_stub);

				// Frontend UI gametype list.
				utils::hook::call(game::game_offset(0x102DB705), DB_FindXAssetHeader_Internal_gametype_stub);
				utils::hook::call(game::game_offset(0x102DB7E3), DB_FindXAssetHeader_Internal_gametype_stub);

				// Keep command registration in command.cpp if this dump helper is needed later.
			}

			void pre_destroy() override
			{
				loaded_gametype_rawfiles.clear();
			}
		};
	}
}

REGISTER_COMPONENT(gametypes::component)
