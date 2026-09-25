#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "component/utils/scheduler.hpp"
#include "component/engine/scripting/gametypes.hpp"
#include "fastfiles.hpp"
#include "xenon.hpp"
#include "component/engine/console/command.hpp"

#include <utils/hook.hpp>
#include <utils/flags.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

#include <unordered_set>
#include <cstring>

namespace fastfiles
{
	namespace
	{
		utils::hook::detour db_link_xasset_entry_hook;
		utils::hook::detour db_create_default_asset_hook;
		utils::hook::detour create_file_a_hook;
		utils::hook::detour db_load_xassets_hook;
		utils::hook::detour db_load_xasset_hook;
		utils::hook::detour db_load_cmodel_hook;
		utils::hook::detour db_load_map_ents_hook;
		utils::hook::detour cm_load_map_hook;
		utils::hook::detour cm_world_init_hook;
		utils::hook::detour sv_game_init_hook;
		utils::hook::detour reflection_probe_nearest_hook;
		void* db_create_default_asset_original = nullptr;
		void* cm_load_map_original = nullptr;
		void* db_load_xasset_original = nullptr;
		void* db_load_cmodel_original = nullptr;
		void* db_load_map_ents_original = nullptr;
		void* reflection_probe_nearest_original = nullptr;
		std::uintptr_t renderer_surface_visibility_continue = 0;
		std::uintptr_t renderer_surface_remap_continue = 0;
		std::uintptr_t renderer_surface_list_continue = 0;
		std::uintptr_t renderer_surface_list_return = 0;
		std::uintptr_t gfx_world_pointer_address = 0;
		std::uintptr_t cg_initialized_address = 0;

		bool common_fastfiles_seen = false;
		bool patch_consolation_loaded = false;
		bool patch_mp_loaded = false;
		bool patch_consolation_attempted = false;
		bool patch_mp_attempted = false;
		char normalized_rawfile_names[1024][256]{};
		unsigned int normalized_rawfile_name_index = 0;
		std::mutex external_asset_log_mutex;
		std::unordered_set<std::string> logged_external_assets;

		bool debug_xasset()
		{
			return utils::flags::has_flag("debug_xasset");
		}

		void __cdecl log_default_asset_creation(const int type, const char* name)
		{
			game::Com_Printf(16, "^3[fastfiles] creating default for missing asset type=%d name=%s\n",
				type, name ? name : "<null>");
		}

		__declspec(naked) void db_create_default_asset_stub()
		{
			__asm
			{
				pushfd
				pushad
				push dword ptr[esp + 0x28]
				push eax
				call log_default_asset_creation
				add esp, 8
				popad
				popfd
				jmp dword ptr[db_create_default_asset_original]
			}
		}

		__declspec(naked) void renderer_surface_list_guard_stub()
		{
			__asm
			{
				// QoS PC 1.1 sub_1036E6F0 can retain a non-zero surface count
				// after its TLS list has been released during a Xenon map teardown.
				// During an active converted map, use the serialized GfxWorld DPVS
				// remap table when the PC-only TLS alias was never initialized.
				mov edx, dword ptr[edi + 1Ch]
				mov eax, dword ptr[eax + 20h]
				test eax, eax
				jnz have_list
				mov eax, dword ptr[cg_initialized_address]
				test eax, eax
				jz no_list
				cmp dword ptr[eax], 0
				jz no_list
				mov eax, dword ptr[gfx_world_pointer_address]
				test eax, eax
				jz no_list
				mov eax, dword ptr[eax]
				test eax, eax
				jz no_list
				mov eax, dword ptr[eax + 2B8h]
				test eax, eax
				jz no_list

			have_list:
				jmp dword ptr[renderer_surface_list_continue]

			no_list:
				jmp dword ptr[renderer_surface_list_return]
			}
		}

		__declspec(naked) void reflection_probe_nearest_stub()
		{
			__asm
			{
				// QoS PC 1.1 passes the position in EDI, followed by GfxWorld and
				// GfxCell on the stack. Reduced Xenon worlds intentionally omit the
				// PC reflection-probe origin array, so an indexed cell must degrade to
				// the default probe instead of dereferencing GfxWorld::reflectionProbes.
				mov eax, dword ptr[esp + 8]
				test eax, eax
				jz no_probe
				cmp byte ptr[eax + 2Ch], 0
				jz no_probe
				mov eax, dword ptr[esp + 4]
				test eax, eax
				jz no_probe
				cmp dword ptr[eax + 10Ch], 0
				jz no_probe
				jmp dword ptr[reflection_probe_nearest_original]

			no_probe:
				xor eax, eax
				ret
			}
		}

		void __cdecl log_cm_load_map_start()
		{
			game::Com_Printf(16, "^5[map-stage] CM_LoadMap enter\n");
		}

		__declspec(naked) void cm_load_map_stub()
		{
			// CM_LoadMap takes the BSP name in ESI and an output pointer on the stack.
			__asm
			{
				pushfd
				pushad
				call log_cm_load_map_start
				popad
				popfd
				jmp dword ptr[cm_load_map_original]
			}
		}

		void __cdecl log_map_asset_load(const int returning)
		{
			// QoS PC 1.1 DB_LoadXAsset (0x103DD0A0) uses this global to
			// identify the current 8-byte asset-table entry.
			const auto entry = *reinterpret_cast<const std::uintptr_t*>(game::game_offset(0x10AB8D14));
			if (!entry)
			{
				return;
			}
			const auto type = *reinterpret_cast<const std::uint32_t*>(entry);
			if (type == game::ASSET_TYPE_COMWORLD || type == game::ASSET_TYPE_GFXWORLD
				|| type == game::ASSET_TYPE_gameWORLD_MP || type == game::ASSET_TYPE_CLIPMAP_MP)
			{
				game::Com_Printf(16, "^5[map-stage] DB_LoadXAsset %s type=%u token=0x%08X entry=%p\n",
					returning ? "returned" : "enter", type,
					*reinterpret_cast<const std::uint32_t*>(entry + 4), reinterpret_cast<const void*>(entry));
			}
		}

		void __cdecl log_clip_nested_load(const int stage, const int returning)
		{
			const auto entry = *reinterpret_cast<const std::uintptr_t*>(game::game_offset(0x10AB8D14));
			if (entry && *reinterpret_cast<const std::uint32_t*>(entry) == game::ASSET_TYPE_CLIPMAP_MP)
			{
				game::Com_Printf(16, "^5[map-stage] clipMap %s %s\n",
						stage ? "MapEnts" : "cmodels", returning ? "returned" : "enter");
			}
		}

		__declspec(naked) void db_load_cmodel_stub()
		{
			__asm
			{
				pushfd
				pushad
				push 0
				push 0
				call log_clip_nested_load
				add esp, 8
				popad
				popfd
				call dword ptr[db_load_cmodel_original]
				pushfd
				pushad
				push 1
				push 0
				call log_clip_nested_load
				add esp, 8
				popad
				popfd
				ret
			}
		}

		__declspec(naked) void db_load_map_ents_stub()
		{
			__asm
			{
				pushfd
				pushad
				push 0
				push 1
				call log_clip_nested_load
				add esp, 8
				popad
				popfd
				call dword ptr[db_load_map_ents_original]
				pushfd
				pushad
				push 1
				push 1
				call log_clip_nested_load
				add esp, 8
				popad
				popfd
				ret
			}
		}

		__declspec(naked) void db_load_xasset_stub()
		{
			__asm
			{
				pushfd
				pushad
				push 0
				call log_map_asset_load
				add esp, 4
				popad
				popfd
				call dword ptr[db_load_xasset_original]
				pushfd
				pushad
				push 1
				call log_map_asset_load
				add esp, 4
				popad
				popfd
				ret
			}
		}

		int cm_world_init_stub()
		{
			game::Com_Printf(16, "^5[map-stage] CM_LoadMap returned; world init enter\n");
			const auto result = cm_world_init_hook.invoke<int>();
			game::Com_Printf(16, "^5[map-stage] world init returned\n");
			return result;
		}

		int sv_game_init_stub(const int arg1, const int arg2)
		{
			game::Com_Printf(16, "^5[map-stage] SV game init enter\n");
			const auto result = sv_game_init_hook.invoke<int>(arg1, arg2);
			game::Com_Printf(16, "^5[map-stage] SV game init returned\n");
			return result;
		}

		bool zone_name_equals(const char* lhs, const char* rhs)
		{
			return lhs != nullptr && rhs != nullptr && _stricmp(lhs, rhs) == 0;
		}

		bool ends_with_ignore_case(const std::string& value, const std::string& suffix)
		{
			if (value.size() < suffix.size())
			{
				return false;
			}

			return _stricmp(value.c_str() + value.size() - suffix.size(), suffix.c_str()) == 0;
		}

		bool is_zone_fastfile_path(const char* file_name)
		{
			if (file_name == nullptr || file_name[0] == '\0')
			{
				return false;
			}

			auto path = std::string(file_name);
			std::replace(path.begin(), path.end(), '/', '\\');

			return ends_with_ignore_case(path, ".ff") && path.find("\\zone\\") != std::string::npos;
		}

		bool is_read_open_request(const DWORD desired_access, const DWORD creation_disposition)
		{
			return (desired_access & (GENERIC_READ | FILE_GENERIC_READ)) != 0
				&& (creation_disposition == OPEN_EXISTING || creation_disposition == OPEN_ALWAYS);
		}

		bool is_scaleform_file_path(const char* file_name)
		{
			if (file_name == nullptr || file_name[0] == '\0')
			{
				return false;
			}

			auto path = std::string(file_name);
			std::replace(path.begin(), path.end(), '/', '\\');
			return (ends_with_ignore_case(path, ".gfx") || ends_with_ignore_case(path, ".swf"))
				&& (path.find("\\scaleform\\") != std::string::npos || path.find("\\ui_mp\\") != std::string::npos);
		}

		bool is_scaleform_asset_name(const char* asset_name)
		{
			if (asset_name == nullptr || asset_name[0] == '\0')
			{
				return false;
			}

			auto name = std::string(asset_name);
			std::replace(name.begin(), name.end(), '\\', '/');
			std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});

			return name.find(".gfx") != std::string::npos
				|| name.find(".swf") != std::string::npos
				|| name.find("scaleform") != std::string::npos
				|| name.find("mpsysmodeselect") != std::string::npos
				|| name.find("mpxbplaylistselect") != std::string::npos
				|| name.find("cmsharedplatform") != std::string::npos
				|| name.find("gfxfontlib") != std::string::npos
				|| name.find("pcsharedlibrary") != std::string::npos
				|| name.find("cmsharedlibrary") != std::string::npos;
		}

		std::filesystem::path get_executable_folder()
		{
			char path[MAX_PATH]{};
			GetModuleFileNameA(nullptr, path, sizeof(path));

			return std::filesystem::path(path).parent_path();
		}

		std::array<std::filesystem::path, 6> zone_lookup_roots()
		{
			const auto host_folder = std::filesystem::path(utils::nt::get_host_module().get_folder());
			const auto exe_folder = get_executable_folder();
			const auto root_folder = std::filesystem::current_path();

			return
			{
				host_folder / "zone",
				host_folder / "consolation" / "zone",
				exe_folder / "zone",
				exe_folder / "consolation" / "zone",
				root_folder / "zone",
				root_folder / "consolation" / "zone",
			};
		}

		std::optional<std::filesystem::path> find_zone_file(const std::string& zone_file_name)
		{
			for (const auto& root : zone_lookup_roots())
			{
				std::error_code ec{};
				if (!std::filesystem::exists(root, ec))
				{
					continue;
				}

				const auto direct_path = root / zone_file_name;
				if (std::filesystem::exists(direct_path, ec))
				{
					return direct_path;
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
						return it->path();
					}
				}
			}

			return std::nullopt;
		}

		std::array<std::filesystem::path, 6> scaleform_lookup_roots()
		{
			const auto host_folder = std::filesystem::path(utils::nt::get_host_module().get_folder());
			const auto exe_folder = get_executable_folder();
			const auto root_folder = std::filesystem::current_path();

			return
			{
				host_folder / "consolation" / "scaleform",
				exe_folder / "consolation" / "scaleform",
				root_folder / "consolation" / "scaleform",
				host_folder / "consolation" / "ui_mp" / "scaleform",
				exe_folder / "consolation" / "ui_mp" / "scaleform",
				root_folder / "consolation" / "ui_mp" / "scaleform",
			};
		}

		std::optional<std::filesystem::path> find_scaleform_file(const char* file_name)
		{
			const auto scaleform_file_name = std::filesystem::path(file_name).filename().string();
			if (scaleform_file_name.empty())
			{
				return std::nullopt;
			}

			for (const auto& root : scaleform_lookup_roots())
			{
				std::error_code ec{};
				const auto direct_path = root / scaleform_file_name;
				if (std::filesystem::exists(direct_path, ec))
				{
					return direct_path;
				}
			}

			return std::nullopt;
		}

		using create_file_a_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

		HANDLE WINAPI create_file_a_stub(const LPCSTR file_name, const DWORD desired_access, const DWORD share_mode,
			const LPSECURITY_ATTRIBUTES security_attributes, const DWORD creation_disposition,
			const DWORD flags_and_attributes, const HANDLE template_file)
		{
			const auto original = reinterpret_cast<create_file_a_t>(create_file_a_hook.get_original());
			// Temporary QoS PC 1.1 probe: verify the map reaches the file-open boundary.
			if (file_name && ends_with_ignore_case(file_name, "mp_canals.ff"))
			{
				game::Com_Printf(16, "^5[Xenon] fastfile open request path=%s access=0x%X share=0x%X disposition=%u flags=0x%X\n",
					file_name, desired_access, share_mode, creation_disposition, flags_and_attributes);
			}
			if (is_read_open_request(desired_access, creation_disposition) && file_name
				&& ends_with_ignore_case(file_name, ".ff"))
			{
				try
				{
					const auto converted = xenon::open_prepared(file_name, desired_access, share_mode,
						security_attributes, creation_disposition, flags_and_attributes, template_file);
					if (converted)
					{
						const auto open_error = *converted == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
						game::Com_Printf(16, "^5[Xenon] opened prepared fastfile for %s (%s, error=%lu)\n",
							file_name, *converted == INVALID_HANDLE_VALUE ? "failed" : "ok", open_error);
						SetLastError(open_error);
						return *converted;
					}
				}
				catch (const std::exception& error)
				{
					game::Com_Printf(16, "^1[Xenon] Open failed: %s\n", error.what());
					SetLastError(ERROR_INVALID_DATA);
					return INVALID_HANDLE_VALUE;
				}
			}

			if (is_read_open_request(desired_access, creation_disposition) && is_scaleform_file_path(file_name))
			{
				game::Com_Printf(16, "^5Opening Scaleform file %s\n", file_name);

				const auto fallback_path = find_scaleform_file(file_name);
				if (fallback_path)
				{
					const auto handle = original(fallback_path->string().c_str(), desired_access, share_mode, security_attributes,
						creation_disposition, flags_and_attributes, template_file);
					if (handle != INVALID_HANDLE_VALUE)
					{
						game::Com_Printf(16, "^5Loading Scaleform override %s\n", fallback_path->string().c_str());

						return handle;
					}
				}
			}

			auto handle = original(file_name, desired_access, share_mode, security_attributes, creation_disposition,
				flags_and_attributes, template_file);

			if (handle != INVALID_HANDLE_VALUE || !is_zone_fastfile_path(file_name))
			{
				return handle;
			}

			const auto original_error = GetLastError();
			const auto zone_file_name = std::filesystem::path(file_name).filename().string();
			const auto fallback_path = find_zone_file(zone_file_name);
			if (!fallback_path)
			{
				SetLastError(original_error);
				return INVALID_HANDLE_VALUE;
			}

			handle = original(fallback_path->string().c_str(), desired_access, share_mode, security_attributes,
				creation_disposition, flags_and_attributes, template_file);
			if (handle == INVALID_HANDLE_VALUE)
			{
				SetLastError(original_error);
			}

			return handle;
		}

		int db_load_xassets_stub(game::XZoneInfo* zones, const int count, const int sync)
		{
			for (int i = 0; zones && i < count; ++i)
			{
				if (zones[i].name && std::string_view(zones[i].name).starts_with("mp_"))
				{
					std::lock_guard lock(external_asset_log_mutex);
					logged_external_assets.clear();
					break;
				}
			}

			// Preflight before native DB_LoadXAssets can unload existing zones or queue IO.
			// QoS PC 1.1, 0x103E1CF0. Remove this adapter when native Xenon schemas exist.
			try
			{
				bool unloads_zones = false;
				for (int i = 0; zones && i < count; ++i) unloads_zones |= zones[i].freeFlags != 0;
				for (int i = 0; zones && i < count; ++i)
				{
					if (!zones[i].name) continue;
					const auto source = find_zone_file(std::string(zones[i].name) + ".ff");
					if (source && xenon::prepare(*source, zones[i].name) && unloads_zones)
						throw std::runtime_error("Xenon UI conversion requires resident PC shader assets; use loadXenonZone without unloading zones");
				}
			}
			catch (const std::exception& error)
			{
				game::Com_Printf(16, "^1[Xenon] Zone batch rejected before native loading: %s\n", error.what());
				return 0;
			}
			return db_load_xassets_hook.invoke<int>(zones, count, sync);
		}

		void load_xenon_zone(const command::params& args)
		{
			if (args.size() != 2)
			{
				game::Com_Printf(16, "loadXenonZone <path.ff>: convert Xenon v470 or load an explicit PC v470 probe\n");
				return;
			}
			try
			{
				const std::filesystem::path source(args[1]);
				const auto name = source.stem().string();
				if (name.empty() || name.size() >= 64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
					throw std::runtime_error("invalid zone name");
				if (!xenon::prepare(source, name, true)) throw std::runtime_error("input is not a recognized v470 fastfile");
				if (name.starts_with("mp_"))
				{
					// Keep map-zone ownership in the normal server transition. An explicit
					// preload followed by devmap causes a second map load after an unload.
					game::Com_Printf(16, "^5[Xenon] Prepared map zone %s; run devmap %s to load it\n",
						name.c_str(), name.c_str());
					return;
				}
				// Native map zones use allocation class 2. Override/patch fastfiles use 0x11.
				game::XZoneInfo zone{name.c_str(), 2, 0};
				// Explicit path is already preflighted, so do not resolve a second file by name.
				db_load_xassets_hook.invoke<int>(&zone, 1, 0);
				game::DB_WaitXAssets.get()();
				game::Com_Printf(16, "^5[Xenon] Native loading completed for %s\n", name.c_str());
			}
			catch (const std::exception& error)
			{
				game::Com_Printf(16, "^1[Xenon] %s\n", error.what());
			}
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

			return find_zone_file(std::string(zone_name) + ".ff").has_value();
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

		__declspec(naked) void renderer_surface_visibility_stub()
		{
			__asm
			{
				// QoS PC 1.1 normally remaps the GfxAabbTree's contiguous surface
				// range through a PC-only uint16 list in TLS. Reduced Xenon worlds
				// do not serialize that list. A low address here is the null base plus
				// firstSurface * 2, so use the already-validated contiguous index.
				cmp edx, 10000h
				jb use_contiguous_index
				movzx eax, word ptr[edx + ecx * 2]
				jmp load_tls

			use_contiguous_index:
				mov eax, dword ptr[ebx + 1Ch]
				add eax, ecx

			load_tls:
				mov edi, fs:[2Ch]
				mov edi, dword ptr[edi + esi * 4]
				jmp dword ptr[renderer_surface_visibility_continue]
			}
		}

		__declspec(naked) void renderer_surface_remap_stub()
		{
			__asm
			{
				// A converted Xenon GfxWorld may omit the PC surface-remap table.
				// This loop already has (firstSurface + index) * 2 in EBP; when the
				// optional remap pointer is absent, use that contiguous surface index.
				mov eax, dword ptr[edx + 2B8h]
				test eax, eax
				jz use_contiguous_index
				movzx esi, word ptr[eax + ebp]
				jmp dword ptr[renderer_surface_remap_continue]

			use_contiguous_index:
				mov esi, ebp
				shr esi, 1
				jmp dword ptr[renderer_surface_remap_continue]
			}
		}

		void log_canals_world_materials(const game::XAssetEntry* entry)
		{
			// Temporary QoS PC 1.1 diagnostic for the generated Xenon map probe.
			// The offsets and 48-byte surface stride come from the native PC
			// GfxWorld loader at 0x103D8960 and its surface loader.
			if (!entry || !entry->asset.header.data)
			{
				return;
			}

			const auto* const world = static_cast<const unsigned char*>(entry->asset.header.data);
			const auto surface_count = *reinterpret_cast<const unsigned int*>(world + 32);
			const auto* const surfaces = *reinterpret_cast<const unsigned char* const*>(world + 36);
			if (!surfaces || surface_count > 100000)
			{
				game::Com_Printf(16, "^1[canals-materials] invalid surface table count=%u surfaces=%p\n",
					surface_count, surfaces);
				return;
			}

			unsigned int null_materials = 0;
			unsigned int white_materials = 0;
			unsigned int textured_materials = 0;
			std::unordered_set<std::string> samples;
			for (unsigned int index = 0; index < surface_count; ++index)
			{
				const auto* const material = *reinterpret_cast<game::Material* const*>(
					surfaces + index * 48 + 16);
				if (!material || !material->name)
				{
					++null_materials;
					continue;
				}

				if (!_stricmp(material->name, "white") || !_stricmp(material->name, ",white"))
				{
					++white_materials;
				}
				else
				{
					++textured_materials;
				}

				if (samples.size() < 12 && samples.emplace(material->name).second)
				{
					const auto* const technique_name = material->techniqueSet && material->techniqueSet->name
						? material->techniqueSet->name : "<null>";
					const char* image_name = "<none>";
					if (material->textureCount > 0 && material->textureTable && material->textureTable[0].image)
					{
						const auto* const image = static_cast<const unsigned char*>(material->textureTable[0].image);
						const auto* const loaded_name = *reinterpret_cast<const char* const*>(image + 32);
						if (loaded_name)
						{
							image_name = loaded_name;
						}
					}

					game::Com_Printf(16,
						"^5[canals-materials] sample=%s techset=%s textures=%u image0=%s\n",
						material->name, technique_name,
						static_cast<unsigned int>(static_cast<unsigned char>(material->textureCount)),
						image_name);

					if (samples.size() <= 4)
					{
						unsigned int technique_count = 0;
						std::string techniques;
						if (material->techniqueSet)
						{
							for (unsigned int technique_index = 0; technique_index < 43; ++technique_index)
							{
								const auto* const technique = material->techniqueSet->techniques[technique_index];
								if (!technique)
								{
									continue;
								}

								++technique_count;
								if (techniques.size() < 220)
								{
									if (!techniques.empty())
									{
										techniques += ",";
									}

									techniques += utils::string::va("%s:%u",
										technique->name ? technique->name : "<unnamed>",
										static_cast<unsigned int>(technique->numPasses));
								}
							}
						}

						game::Com_Printf(16,
							"^5[canals-materials] technique-set=%s ptr=%p worldFormat=%u techniques=%u {%s}\n",
							technique_name, material->techniqueSet,
							material->techniqueSet
								? static_cast<unsigned int>(static_cast<unsigned char>(material->techniqueSet->worldVertFormat))
								: 0,
							technique_count, techniques.c_str());
					}

					if (samples.size() <= 4 && material->textureTable)
					{
						const auto texture_count = static_cast<unsigned int>(
							static_cast<unsigned char>(material->textureCount));
						for (unsigned int texture_index = 0; texture_index < texture_count; ++texture_index)
						{
							const auto& texture = material->textureTable[texture_index];
							const auto* const image = static_cast<const unsigned char*>(texture.image);
							const auto* const texture_image_name = image
								? *reinterpret_cast<const char* const*>(image + 32) : nullptr;
							const auto gpu_texture = image
								? *reinterpret_cast<void* const*>(image + 4) : nullptr;
							const auto width = image
								? *reinterpret_cast<const unsigned short*>(image + 24) : 0;
							const auto height = image
								? *reinterpret_cast<const unsigned short*>(image + 26) : 0;
							game::Com_Printf(16,
								"^5[canals-materials] texture material=%s slot=%u hash=0x%08X semantic=%u image=%s gpu=%p size=%ux%u\n",
								material->name, texture_index, texture.typeHash,
								static_cast<unsigned int>(static_cast<unsigned char>(texture.semantic)),
								texture_image_name ? texture_image_name : "<null>", gpu_texture,
								static_cast<unsigned int>(width), static_cast<unsigned int>(height));
						}
					}
				}
			}

			game::Com_Printf(16,
				"^5[canals-materials] surfaces=%u textured=%u white=%u null=%u unique-samples=%zu\n",
				surface_count, textured_materials, white_materials, null_materials, samples.size());
		}

		game::XAssetEntry* db_link_xasset_entry_stub(game::XAssetEntry* entry, const int allow_override)
		{
			normalize_rawfile_name(entry);

			const auto* const incoming_name = get_asset_name(entry);
			const auto type = entry ? static_cast<int>(entry->asset.type) : -1;
			// Temporary QoS PC 1.1 probe for the generated mp_canals zone. Remove
			// once native map asset linking and CM_LoadMap have been verified.
			const bool trace_map_asset = entry && (type == game::ASSET_TYPE_gameWORLD_MP
				|| type == game::ASSET_TYPE_CLIPMAP_MP || type == game::ASSET_TYPE_MAP_ENTS
				|| ((type == game::ASSET_TYPE_GFXWORLD || type == game::ASSET_TYPE_COMWORLD)
					&& std::strstr(incoming_name, "mp_canals") != nullptr));
			if (trace_map_asset)
			{
				game::Com_Printf(16, "^5[map-stage] link enter type=%d name=%s\n", type, incoming_name);
			}
			if (incoming_name[0] == ',')
			{
				const auto key = std::to_string(static_cast<int>(entry->asset.type)) + ":" + incoming_name;
				bool first_reference = false;
				{
					std::lock_guard lock(external_asset_log_mutex);
					first_reference = logged_external_assets.emplace(key).second;
				}
				if (first_reference)
				{
					game::Com_Printf(16, "^5[fastfiles] resolving external asset type=%d name=%s\n",
						static_cast<int>(entry->asset.type), incoming_name + 1);
				}
			}
			auto* const linked_entry = db_link_xasset_entry_hook.invoke<game::XAssetEntry*>(entry, allow_override);
			if (trace_map_asset)
			{
				game::Com_Printf(16, "^5[map-stage] link returned type=%d name=%s linked=%p\n",
					type, incoming_name, linked_entry);
				if (type == game::ASSET_TYPE_GFXWORLD && std::strstr(incoming_name, "mp_canals") != nullptr)
				{
					log_canals_world_materials(linked_entry ? linked_entry : entry);
				}
			}
			auto* const log_entry = linked_entry ? linked_entry : entry;
			if (log_entry)
			{
				const auto incoming_zone_index = get_asset_zone_index(entry);
				const auto* const incoming_zone_name = get_zone_name(incoming_zone_index);
				const auto zone_index = get_asset_zone_index(log_entry);
				const auto* const zone_name = get_zone_name(zone_index);
				const auto* const linked_name = get_asset_name(log_entry);
				common_fastfiles_seen = common_fastfiles_seen || zone_name_equals(incoming_zone_name, "common_mp") || zone_name_equals(zone_name, "common_mp");
				patch_mp_loaded = patch_mp_loaded || zone_name_equals(incoming_zone_name, "patch_mp") || zone_name_equals(zone_name, "patch_mp");
				patch_consolation_loaded = patch_consolation_loaded || zone_name_equals(incoming_zone_name, "patch_consolation") || zone_name_equals(zone_name, "patch_consolation");

				if (is_scaleform_asset_name(incoming_name) || is_scaleform_asset_name(linked_name))
				{
					game::Com_Printf(16, "^5[scaleform-xasset] ent=%p, link=%p, t=%d, n=%s, lN=%s, eZ=%u:%s, lZ=%u:%s, lZF=0x%X, allowOverride=%d)\n",
						entry,
						linked_entry,
						static_cast<int>(log_entry->asset.type),
						incoming_name,
						linked_name,
						incoming_zone_index,
						incoming_zone_name,
						zone_index,
						zone_name,
						get_zone_flags(zone_index),
						allow_override);
				}

				if (debug_xasset())
				{
					game::Com_Printf(16, "^5(ent=%p, link=%p, t=%d, n=%s, lN=%s, eZ=%u:%s, lZ=%u:%s, lZF=0x%X, allowOverride=%d)\n",
						entry,
						linked_entry,
						static_cast<int>(log_entry->asset.type),
						incoming_name,
						linked_name,
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
					game::Com_Printf(16, "^1Skipping override fastfile 'patch_mp' because it was not found\n");
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
					game::Com_Printf(16, "^1Skipping override fastfile 'patch_consolation' because it was not found\n");
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
			if (patch_consolation_expected)
			{
				gametypes::refresh_ui_gametype_list();
			}

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
			gfx_world_pointer_address = game::game_offset(0x10C4A354);
			cg_initialized_address = game::game_offset(0x129FE8E4);
			// sub_103A4840 assumes every cell probe index has a matching world
			// origin array. Generated reduced worlds do not serialize that PC-only
			// array yet, so preserve native lookup only when the array exists.
			reflection_probe_nearest_hook.create(game::game_offset(0x103A4840), reflection_probe_nearest_stub);
			reflection_probe_nearest_original = reflection_probe_nearest_hook.get_original();
			// Exit-time dump: 0x103678C9 read 0x13BC through a missing Xenon-world
			// surface-remap table. Reuse the tree's contiguous firstSurface index.
			renderer_surface_remap_continue = game::game_offset(0x103678CD);
			utils::hook::nop(game::game_offset(0x103678C3), 10);
			utils::hook::jump(game::game_offset(0x103678C3), renderer_surface_remap_stub);
			// Runtime crash evidence: 0x1036E67D read 0x00000E80 while traversing
			// a valid converted cell tree. Fall back to its contiguous surface range
			// only when the optional PC remap-list base is absent.
			renderer_surface_visibility_continue = game::game_offset(0x1036E68B);
			utils::hook::nop(game::game_offset(0x1036E67D), 14);
			utils::hook::jump(game::game_offset(0x1036E67D), renderer_surface_visibility_stub);
			// Runtime crash evidence: 0x1036E769 read [0x00000B5A] with a null
			// TLS surface-list base at the end of mp_canals. Preserve native work
			// when the list exists and skip only the stale-list iteration.
			renderer_surface_list_continue = game::game_offset(0x1036E75C);
			renderer_surface_list_return = game::game_offset(0x1036E780);
			utils::hook::nop(game::game_offset(0x1036E756), 6);
			utils::hook::jump(game::game_offset(0x1036E756), renderer_surface_list_guard_stub);
			create_file_a_hook.create(reinterpret_cast<void*>(CreateFileA), create_file_a_stub);
			db_load_xassets_hook.create(game::DB_LoadXAssets, db_load_xassets_stub);
			db_link_xasset_entry_hook.create(game::DB_LinkXAssetEntry, db_link_xasset_entry_stub);
			// QoS PC 1.1 DB_CreateDefaultEntry uses EAX for type and one stack name argument.
			// Diagnostic only: forward unchanged so missing defaults still fail natively.
			db_create_default_asset_hook.create(game::game_offset(0x103E0120), db_create_default_asset_stub);
			db_create_default_asset_original = db_create_default_asset_hook.get_original();
			// Temporary QoS PC 1.1 probes. Remove after the Xenon clipMap and
			// server-game path have both been verified against native map startup.
			cm_load_map_hook.create(game::game_offset(0x103ED060), cm_load_map_stub);
			cm_load_map_original = cm_load_map_hook.get_original();
			// Temporary loader-boundary probe for the mp_canals root stream.
			db_load_xasset_hook.create(game::game_offset(0x103DD0A0), db_load_xasset_stub);
			db_load_xasset_original = db_load_xasset_hook.get_original();
			db_load_cmodel_hook.create(game::game_offset(0x103CFAA0), db_load_cmodel_stub);
			db_load_cmodel_original = db_load_cmodel_hook.get_original();
			db_load_map_ents_hook.create(game::game_offset(0x103D15D0), db_load_map_ents_stub);
			db_load_map_ents_original = db_load_map_ents_hook.get_original();
			cm_world_init_hook.create(game::game_offset(0x101AF9F0), cm_world_init_stub);
			sv_game_init_hook.create(game::game_offset(0x102F5920), sv_game_init_stub);
			command::add("loadXenonZone", load_xenon_zone);
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

		void pre_destroy() override
		{
			xenon::clear();
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
