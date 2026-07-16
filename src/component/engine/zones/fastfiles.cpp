#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "fastfiles.hpp"

#include <utils/hook.hpp>
#include <utils/nt.hpp>

namespace fastfiles
{
	namespace
	{
		utils::hook::detour db_load_xassets_hook;

		bool patch_consolation_loaded = false;
		bool patch_mp_loaded = false;
		bool patch_consolation_attempted = false;
		bool patch_mp_attempted = false;

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
			zone_info.allocFlags = 2;
			zone_info.freeFlags = 0;

			return zone_info;
		}

		int db_load_xassets_stub(game::XZoneInfo* zone_info, const int zone_count, const int sync)
		{
			patch_consolation_loaded = patch_consolation_loaded || has_zone(zone_info, zone_count, "patch_consolation");
			patch_mp_loaded = patch_mp_loaded || has_zone(zone_info, zone_count, "patch_mp");

			const auto loading_common_fastfiles = has_zone(zone_info, zone_count, "common_mp");
			const auto should_load_patch_consolation = loading_common_fastfiles
				&& !patch_consolation_attempted
				&& !patch_consolation_loaded;

			const auto should_load_patch_mp = loading_common_fastfiles
				&& !patch_mp_attempted
				&& !patch_mp_loaded;

			const auto result = db_load_xassets_hook.invoke<int>(zone_info, zone_count, sync);
			if (!result)
			{
				return result;
			}

			std::vector<game::XZoneInfo> patch_zones{};
			if (should_load_patch_mp)
			{
				patch_mp_attempted = true;
				if (zone_file_exists("patch_mp"))
				{
					patch_zones.push_back(make_override_zone("patch_mp"));
					patch_mp_loaded = true;
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
					patch_consolation_loaded = true;
				}
				else
				{
					game::Com_Printf(16, "Skipping override fastfile 'patch_consolation' because it was not found\n");
				}
			}

			if (!patch_zones.empty())
			{
				game::Com_Printf(16, "Loading patch fastfiles with override priority\n");
				db_load_xassets_hook.invoke<int>(patch_zones.data(), static_cast<int>(patch_zones.size()), sync);
			}

			return result;
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
			db_load_xassets_hook.create(game::DB_LoadXAssets, db_load_xassets_stub);
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
