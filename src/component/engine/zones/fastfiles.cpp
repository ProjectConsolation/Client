#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "component/utils/scheduler.hpp"
#include "fastfiles.hpp"

#include <utils/hook.hpp>
#include <utils/flags.hpp>
#include <utils/nt.hpp>

namespace fastfiles
{
	namespace
	{
		utils::hook::detour db_link_xasset_entry_hook;

		bool common_fastfiles_seen = false;
		bool patch_consolation_loaded = false;
		bool patch_mp_loaded = false;
		bool patch_consolation_attempted = false;
		bool patch_mp_attempted = false;
		char normalized_rawfile_names[1024][256]{};
		unsigned int normalized_rawfile_name_index = 0;

		bool debug_xasset()
		{
			return utils::flags::has_flag("debug_xasset");
		}

		bool zone_name_equals(const char* lhs, const char* rhs)
		{
			return lhs != nullptr && rhs != nullptr && _stricmp(lhs, rhs) == 0;
		}

		bool has_zone(const game::XZoneInfo* zone_info, const int zone_count, const char* name)
		{
			if (zone_info == nullptr || zone_count <= 0 || name == nullptr)
			{
				return false;
			}

			for (auto index = 0; index < zone_count; ++index)
			{
				if (zone_name_equals(zone_info[index].name, name))
				{
					return true;
				}
			}

			return false;
		}

		bool zone_file_exists(const char* zone_name)
		{
			if (zone_name == nullptr || zone_name[0] == '\0')
			{
				return false;
			}

			const auto zone_file_name = std::string(zone_name) + ".ff";
			const std::array roots
			{
				std::filesystem::path(utils::nt::get_host_module().get_folder()) / "zone",
				std::filesystem::current_path() / "zone",
			};

			for (const auto& root : roots)
			{
				std::error_code ec{};
				if (!std::filesystem::exists(root, ec))
				{
					continue;
				}

				if (std::filesystem::exists(root / zone_file_name, ec))
				{
					return true;
				}

				for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
				{
					if (!it->is_regular_file(ec))
					{
						continue;
					}

					const auto filename = it->path().filename().string();
					if (_stricmp(filename.c_str(), zone_file_name.c_str()) == 0)
					{
						return true;
					}
				}
			}

			return false;
		}

		game::XZoneInfo make_override_zone(const char* zone_name)
		{
			game::XZoneInfo zone_info{};
			zone_info.name = zone_name;
			zone_info.allocFlags = 0x11;
			zone_info.freeFlags = 0;

			return zone_info;
		}

		std::string join_zone_names(const std::vector<game::XZoneInfo>& zones)
		{
			std::string names{};
			for (const auto& zone : zones)
			{
				if (!names.empty())
				{
					names += ", ";
				}

				names += zone.name;
			}

			return names;
		}

		void print_zone_list(const char* prefix, const game::XZoneInfo* zone_info, const int zone_count, const int sync)
		{
			if (!debug_xasset())
			{
				return;
			}

			game::Com_Printf(16, "^5%s zoneCount=%d sync=%d\n", prefix, zone_count, sync);

			if (zone_info == nullptr || zone_count <= 0)
			{
				return;
			}

			for (auto index = 0; index < zone_count; ++index)
			{
				game::Com_Printf(16, "^5  [%d] name=%s allocFlags=0x%X freeFlags=0x%X\n",
					index,
					zone_info[index].name ? zone_info[index].name : "<null>",
					zone_info[index].allocFlags,
					zone_info[index].freeFlags);
			}
		}

		const char* get_asset_name(const game::XAssetEntry* entry)
		{
			if (!entry || !entry->asset.header.data)
			{
				return "<null>";
			}

			const auto type_index = static_cast<int>(entry->asset.type);
			if (type_index < 0 || type_index >= game::ASSET_TYPE_COUNT)
			{
				return "<invalid>";
			}

			auto asset = entry->asset;
			const auto* const name = game::DB_GetXAssetName(&asset);
			return name && name[0] ? name : "<unnamed>";
		}

		void print_zone_load_state(const char* action, const std::vector<game::XZoneInfo>& zones)
		{
			for (const auto& zone : zones)
			{
				if (!zone.name)
				{
					continue;
				}

				if (debug_xasset() && _stricmp(action, "Loading") == 0)
				{
					game::Com_Printf(16, "^5Loading fastfile '%s'\n", zone.name);
				}
				else
				{
					game::Com_Printf(16, "^5%s zone '%s'\n", action, zone.name);
				}
			}
		}

		void normalize_rawfile_name(game::XAssetEntry* entry)
		{
			if (!entry || entry->asset.type != game::ASSET_TYPE_RAWFILE || !entry->asset.header.rawfile || !entry->asset.header.rawfile->name)
			{
				return;
			}

			const auto* const name = entry->asset.header.rawfile->name;
			if (!std::strchr(name, '\\'))
			{
				return;
			}

			auto* const normalized_name = normalized_rawfile_names[normalized_rawfile_name_index++ % 1024];
			strncpy_s(normalized_name, sizeof(normalized_rawfile_names[0]), name, _TRUNCATE);

			for (auto* current = normalized_name; *current; ++current)
			{
				if (*current == '\\')
				{
					*current = '/';
				}
			}

			entry->asset.header.rawfile->name = normalized_name;
		}

		unsigned char get_asset_zone_index(const game::XAssetEntry* entry)
		{
			return entry ? static_cast<unsigned char>(entry->zoneIndex) : 0;
		}

		const char* get_zone_name(const unsigned char zone_index)
		{
			const auto* const zones = reinterpret_cast<game::XZone*>(game::game_offset(0x10AB8188));
			const auto* const zone = &zones[zone_index];
			return zone->name[0] ? zone->name : "<none>";
		}

		int get_zone_flags(const unsigned char zone_index)
		{
			const auto* const zones = reinterpret_cast<game::XZone*>(game::game_offset(0x10AB8188));
			return zones[zone_index].flags;
		}

		game::XAssetEntry* db_link_xasset_entry_stub(game::XAssetEntry* entry, const int allow_override)
		{
			normalize_rawfile_name(entry);

			const auto* const incoming_name = get_asset_name(entry);
			auto* const linked_entry = db_link_xasset_entry_hook.invoke<game::XAssetEntry*>(entry, allow_override);
			auto* const log_entry = linked_entry ? linked_entry : entry;
			if (log_entry)
			{
				const auto incoming_zone_index = get_asset_zone_index(entry);
				const auto* const incoming_zone_name = get_zone_name(incoming_zone_index);
				const auto zone_index = get_asset_zone_index(log_entry);
				const auto* const zone_name = get_zone_name(zone_index);
				common_fastfiles_seen = common_fastfiles_seen || zone_name_equals(incoming_zone_name, "common_mp") || zone_name_equals(zone_name, "common_mp");
				patch_mp_loaded = patch_mp_loaded || zone_name_equals(incoming_zone_name, "patch_mp") || zone_name_equals(zone_name, "patch_mp");
				patch_consolation_loaded = patch_consolation_loaded || zone_name_equals(incoming_zone_name, "patch_consolation") || zone_name_equals(zone_name, "patch_consolation");

				if (debug_xasset())
				{
					game::Com_Printf(16, "^5(ent=%p, link=%p, t=%d, n=%s, lN=%s, eZ=%u:%s, lZ=%u:%s, lZF=0x%X, allowOverride=%d)\n",
						entry,
						linked_entry,
						static_cast<int>(log_entry->asset.type),
						incoming_name,
						get_asset_name(log_entry),
						incoming_zone_index,
						incoming_zone_name,
						zone_index,
						zone_name,
						get_zone_flags(zone_index),
						allow_override);
				}
			}

			return linked_entry;
		}

		std::vector<game::XZoneInfo> get_pending_patch_zones()
		{
			const auto should_load_patch_consolation = common_fastfiles_seen
				&& !patch_consolation_attempted
				&& !patch_consolation_loaded;

			const auto should_load_patch_mp = common_fastfiles_seen
				&& !patch_mp_attempted
				&& !patch_mp_loaded;

			std::vector<game::XZoneInfo> patch_zones{};
			if (should_load_patch_mp)
			{
				patch_mp_attempted = true;
				if (zone_file_exists("patch_mp"))
				{
					patch_zones.push_back(make_override_zone("patch_mp"));
				}
				else
				{
					game::Com_Printf(16, "Skipping override fastfile 'patch_mp' because it was not found\n");
				}
			}

			if (should_load_patch_consolation)
			{
				patch_consolation_attempted = true;
				if (zone_file_exists("patch_consolation"))
				{
					patch_zones.push_back(make_override_zone("patch_consolation"));
				}
				else
				{
					game::Com_Printf(16, "Skipping override fastfile 'patch_consolation' because it was not found\n");
				}
			}

			return patch_zones;
		}

		void load_patch_fastfiles_after_common()
		{
			if (!common_fastfiles_seen)
			{
				return;
			}

			auto patch_zones = get_pending_patch_zones();
			if (patch_zones.empty())
			{
				return;
			}

			const auto patch_sync = 0;
			const auto patch_zone_names = join_zone_names(patch_zones);
			print_zone_load_state("Loading", patch_zones);
			print_zone_list("DB_LoadPatchFastFiles", patch_zones.data(), static_cast<int>(patch_zones.size()), patch_sync);
			game::DB_LoadXAssets.get()(patch_zones.data(), static_cast<int>(patch_zones.size()), patch_sync);
			game::DB_WaitXAssets.get()();

			const auto patch_mp_expected = has_zone(patch_zones.data(), static_cast<int>(patch_zones.size()), "patch_mp");
			const auto patch_consolation_expected = has_zone(patch_zones.data(), static_cast<int>(patch_zones.size()), "patch_consolation");
			if ((!patch_mp_expected || patch_mp_loaded) && (!patch_consolation_expected || patch_consolation_loaded))
			{
				print_zone_load_state("Loaded", patch_zones);
			}
			else
			{
				game::Com_Printf(16, "^3Submitted patch fastfile(s), but no linked assets were observed yet: %s\n", patch_zone_names.c_str());
			}
		}
	}

	void enum_assets(const game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, const bool include_override)
	{
		game::DB_EnumXAssets_FastFile(type, static_cast<void(*)(game::XAssetHeader, void*)>([](game::XAssetHeader header, void* data)
			{
				const auto& cb = *static_cast<const std::function<void(game::XAssetHeader)>*>(data);
				cb(header);
			}), &callback, include_override);
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			db_link_xasset_entry_hook.create(game::DB_LinkXAssetEntry, db_link_xasset_entry_stub);
			scheduler::schedule([]()
			{
				if (!common_fastfiles_seen)
				{
					return scheduler::cond_continue;
				}

				load_patch_fastfiles_after_common();
				return scheduler::cond_end;
			}, scheduler::main, 250ms);
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
