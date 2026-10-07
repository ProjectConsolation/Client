#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "component/utils/scheduler.hpp"
#include "component/engine/scripting/gametypes.hpp"
#include "fastfiles.hpp"
#include "xenon.hpp"
#include "dlc_zone_policy.hpp"
#include "portal_links.hpp"
#include "component/engine/console/command.hpp"

#include <utils/hook.hpp>
#include <utils/flags.hpp>
#include <utils/memory.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

#include <unordered_set>
#include <cstring>
#include <unordered_map>
#include <stdexcept>

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
		utils::hook::detour renderer_cell_job_hook;
		volatile LONG renderer_cell_jobs = 0;
		volatile LONG renderer_bulk_surface_marks = 0;
		volatile LONG renderer_leaf_surface_lists = 0;
		volatile LONG renderer_last_surface_enabled = 0;
		volatile LONG renderer_last_cell = 0;
		volatile LONG renderer_last_view = 0;
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
		std::uintptr_t renderer_reflection_probe_continue = 0;
		std::uintptr_t renderer_reflection_probe_skip = 0;
		std::uintptr_t renderer_reflection_probe_secondary_continue = 0;
		std::uintptr_t renderer_reflection_probe_secondary_skip = 0;
		std::uintptr_t gfx_world_pointer_address = 0;
		std::uintptr_t cg_initialized_address = 0;

		bool common_fastfiles_seen = false;
		bool common_consolation_loaded = false;
		bool common_consolation_attempted = false;
		bool patch_consolation_loaded = false;
		bool patch_mp_loaded = false;
		bool common_xenon_loaded = false;
		bool common_xenon_attempted = false;
		std::atomic_bool common_xenon_preloading = false;
		std::atomic_bool common_xenon_preload_complete = false;
		std::atomic_bool common_xenon_linking = false;
		std::atomic_llong common_xenon_preload_started_ms = 0;
		char common_xenon_preload_zone[64] = "common_xenon";
		bool patch_consolation_attempted = false;
		bool patch_mp_attempted = false;
		char normalized_rawfile_names[1024][256]{};
		unsigned int normalized_rawfile_name_index = 0;
		std::mutex external_asset_log_mutex;
		std::unordered_set<std::string> logged_external_assets;
		std::mutex portal_links_mutex;
		std::unordered_map<std::string, portal_links::manifest> pending_portal_links;

		void relocate_portal_links(game::XAssetEntry* entry)
		{
			if (!entry) return;
			std::lock_guard lock(portal_links_mutex);
			if (entry->asset.type == game::ASSET_TYPE_RAWFILE)
			{
				const auto* file = entry->asset.header.rawfile;
				constexpr std::string_view prefix = "consolation/portal-links/";
				if (file && file->name && file->buffer && std::string_view(file->name).starts_with(prefix))
				{
					const auto zone = std::string(std::string_view(file->name).substr(prefix.size()));
					if (!dlc::requires_donor(zone) || zone.ends_with("_load")) return;
					portal_links::manifest links;
					pending_portal_links.erase(zone);
					if (portal_links::decode({file->buffer, file->len}, links))
						pending_portal_links.emplace(zone, std::move(links));
					else throw std::runtime_error("Invalid DLC portal-link manifest: " + zone);
				}
				return;
			}
			if (entry->asset.type != game::ASSET_TYPE_GFXWORLD) return;
			auto* world = static_cast<unsigned char*>(entry->asset.header.data);
			if (!world) return;
			const auto* base_name = *reinterpret_cast<const char**>(world + 4);
			if (!base_name || !dlc::requires_donor(base_name)) return;
			const std::string zone(base_name);
			const auto found = pending_portal_links.find(zone);
			if (found == pending_portal_links.end()) return; // Stock/older zones have no adapter.
			const auto& manifest = found->second;
			// QoS PC Load_GfxWorld / R_VisitPortalsForCell; cell=52, portal=68.
			// The converter emits null destinations plus this pre-world rawfile.
			// Validate the entire allocation and sequence before publishing pointers.
			if (!world || *reinterpret_cast<std::uint32_t*>(world + 288) != manifest.cells)
				throw std::runtime_error("DLC portal cell count mismatch: " + zone);
			auto* cells = *reinterpret_cast<unsigned char**>(world + 296);
			if (!cells) throw std::runtime_error("DLC portal cells missing: " + zone);
			std::vector<std::pair<unsigned char*, unsigned char*>> writes;
			std::size_t index = 0;
			for (std::uint32_t ci = 0; ci < manifest.cells; ++ci)
			{
				auto* cell = cells + ci * 52;
				const auto count = *reinterpret_cast<std::uint32_t*>(cell + 28);
				auto* portals = *reinterpret_cast<unsigned char**>(cell + 32);
				if (count > manifest.links.size() || (count && !portals))
					throw std::runtime_error("DLC portal array mismatch: " + zone);
				for (std::uint32_t pi = 0; pi < count; ++pi)
				{
					if (index >= manifest.links.size() || manifest.links[index].cell != ci
						|| manifest.links[index].portal != pi)
						throw std::runtime_error("DLC portal sequence mismatch: " + zone);
					writes.emplace_back(portals + pi * 68 + 32,
						cells + manifest.links[index++].destination * 52);
				}
			}
			if (index != manifest.links.size()) throw std::runtime_error("DLC portal count mismatch: " + zone);
			for (const auto& [address, destination] : writes)
				std::memcpy(address, &destination, sizeof(destination));
			game::Com_Printf(16, "^5[Xenon] relocated %u portal links in %s\n",
				static_cast<unsigned int>(writes.size()), zone.c_str());
			pending_portal_links.erase(found);
		}

		constexpr float preload_bar_height = 9.0f;
		float preload_background_color[4] = {0.0f, 0.0f, 0.0f, 0.90f};
		float preload_bar_back_color[4] = {0.32f, 0.32f, 0.32f, 0.90f};
		float preload_bar_fill_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float preload_text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

		long long steady_time_ms()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void draw_common_xenon_preload()
		{
			if (!common_xenon_preloading.load(std::memory_order_acquire))
			{
				return;
			}

			RECT client_rect{};
			const auto window = *game::main_window;
			if (!window || !GetClientRect(window, &client_rect))
			{
				return;
			}

			const auto width = static_cast<float>(client_rect.right - client_rect.left);
			const auto height = static_cast<float>(client_rect.bottom - client_rect.top);
			if (width <= 0.0f || height <= 0.0f)
			{
				return;
			}

			auto* const white = game::Material_RegisterHandle("white");
			auto* font = game::R_RegisterFont("fonts/bigfont");
			if (!font || font->pixelHeight <= 0)
			{
				font = game::R_RegisterFont("fonts/normalFont");
			}
			if (!white || !font)
			{
				return;
			}

			const auto elapsed_ms = std::max(0LL,
				steady_time_ms() - common_xenon_preload_started_ms.load(std::memory_order_acquire));
			game::R_AddCmdDrawStretchPic(0.0f, 0.0f, width, height,
				0.0f, 0.0f, 0.0f, 0.0f, preload_background_color, white, 0);
			static constexpr int dot_sequence[] = {1, 2, 3, 2};
			const auto dot_count = dot_sequence[(elapsed_ms / 300) % std::size(dot_sequence)];
			char label[128]{};
			sprintf_s(label, "Preloading zone %s%.*s",
				common_xenon_preload_zone, dot_count, "...");

			// Match the native multiplayer load-screen proportions: the bar begins
			// near one-sixth of the frame and spans roughly three-fifths of it.
			const auto bar_x = width * 0.17f;
			const auto bar_width = width * 0.60f;
			const auto bar_y = height * 0.88f;
			const auto target_text_height = std::clamp(height * 0.028f, 22.0f, 36.0f);
			const auto text_scale = target_text_height / static_cast<float>(font->pixelHeight);
			const auto text_y = bar_y - 16.0f;
			game::R_AddCmdDrawText(label, 0x7FFFFFFF, font,
				bar_x, text_y, text_scale, text_scale, 0.0f,
				preload_text_color, 0);
			game::R_AddCmdDrawStretchPic(bar_x, bar_y, bar_width,
				preload_bar_height, 0.0f, 0.0f, 0.0f, 0.0f,
				preload_bar_back_color, white, 0);

			// QoS PC exposes DB completion, not byte progress. Animate a left-origin
			// loading fill and hold below 100 percent until both DB queues drain.
			const auto fill_fraction = common_xenon_preload_complete.load(std::memory_order_acquire)
				? 1.0f
				: std::min(0.94f, 0.04f + static_cast<float>(elapsed_ms) / 5000.0f * 0.90f);
			game::R_AddCmdDrawStretchPic(bar_x, bar_y, bar_width * fill_fraction,
				preload_bar_height, 0.0f, 0.0f, 0.0f, 0.0f,
				preload_bar_fill_color, white, 0);
		}

		void present_common_xenon_preload(const char* const zone_name)
		{
			strncpy_s(common_xenon_preload_zone,
				zone_name && *zone_name ? zone_name : "common_xenon", _TRUNCATE);
			common_xenon_preload_started_ms.store(steady_time_ms(), std::memory_order_release);
			common_xenon_preload_complete.store(false, std::memory_order_release);
			common_xenon_preloading.store(true, std::memory_order_release);
			const auto window = *game::main_window;
			if (window && IsWindow(window))
			{
				// QoS PC Con_DrawConsole (0x10311F70) runs CL_DrawScreen and closes
				// the frame through R_EndFrame, where the renderer pipeline executes.
				game::Con_DrawConsole();
			}
		}

		void wait_for_common_xenon_with_ui()
		{
			// QoS PC 1.1 DB_WaitXAssets (0x103DFD80) polls these two counters for
			// the active batch group. Reproduce that verified wait and present a
			// complete menu frame periodically so the preload UI can animate.
			using allocate_batch_group_t = int(*)();
			using get_batch_count_t = int(*)(int);
			using renderer_sync_t = void(*)();
			const auto allocate_batch_group = reinterpret_cast<allocate_batch_group_t>(
				game::game_offset(0x1014F4F0));
			const auto get_pending_count = reinterpret_cast<get_batch_count_t>(
				game::game_offset(0x1014E680));
			const auto get_active_count = reinterpret_cast<get_batch_count_t>(
				game::game_offset(0x1014E660));
			const auto begin_renderer_sync = reinterpret_cast<renderer_sync_t>(
				game::game_offset(0x103BFF00));
			const auto end_renderer_sync = reinterpret_cast<renderer_sync_t>(
				game::game_offset(0x103BF5C0));
			auto& batch_group = *reinterpret_cast<int*>(game::game_offset(0x1055FA7C));
			const auto renderer_state = reinterpret_cast<const std::uintptr_t*>(game::game_offset(0x110546EC));
			auto next_frame_ms = steady_time_ms();

			while (true)
			{
				if (batch_group == -1)
				{
					batch_group = allocate_batch_group();
				}
				const auto active_group = batch_group;
				if (batch_group == -1)
				{
					batch_group = allocate_batch_group();
				}
				if (get_pending_count(batch_group) + get_active_count(active_group) == 0)
				{
					break;
				}

				if (*renderer_state && *reinterpret_cast<const unsigned char*>(*renderer_state + 16))
				{
					begin_renderer_sync();
				}
				Sleep(1);
				if (*renderer_state && *reinterpret_cast<const unsigned char*>(*renderer_state + 16))
				{
					end_renderer_sync();
				}

				const auto now_ms = steady_time_ms();
				if (now_ms >= next_frame_ms)
				{
					game::Con_DrawConsole();
					next_frame_ms = now_ms + 75;
				}
			}

			common_xenon_preload_complete.store(true, std::memory_order_release);
			game::Con_DrawConsole();
		}

		struct asset_pool_extension
		{
			game::XAssetType type;
			unsigned int stock_size;
			unsigned int extended_size;
			std::size_t element_size;
			std::uintptr_t stock_pool_address;
			std::uintptr_t initializer_address;
		};

		constexpr asset_pool_extension asset_pool_extensions[]
		{
			{game::ASSET_TYPE_XMODEL, 640, 1500, 0xF0, 0x108A5BB0, 0x103DECD0},
			{game::ASSET_TYPE_MATERIAL, 1626, 4096, 0x68, 0x10933798, 0x103DEC90},
			{game::ASSET_TYPE_IMAGE, 2800, 4096, 0x24, 0x1091ADD0, 0x103DEC00},
			{game::ASSET_TYPE_WEAPON, 256, 320, 0xACC, 0x1098D360, 0x103DE960},
			{game::ASSET_TYPE_FX, 340, 600, 0x20, 0x1095CD30, 0x103DE920},
			{game::ASSET_TYPE_STRINGTABLE, 5, 80, 0x10, 0x1098CC88, 0x103DE870},
		};

		void extend_sorted_material_storage()

		{
			// KisakCOD Material_Sort shows that the DB pool and renderer's
			// sortedMaterials array are separate allocations. QoS PC 1.1 uses
			// 1,626 slots at 0x10C48960; its unbounded DB enumeration at
			// 0x103DFBA0 overwrites adjacent globals when the extended pool is
			// used. Bags produced 1,673 entries and overwrote rgp.world
			// (0x10C4A354) with the motion_blur material pointer.
			// Redirect both enumeration destinations and every native consumer.
			// Storage supports the 4,096-entry DB pool; this does NOT widen
			// QoS's 11-bit materialSortedIndex (2,048 renderable materials).
			static game::Material* sorted_materials[4096]{};
			static_assert(sizeof(void*) == 4);
			constexpr std::uintptr_t operands[] =
			{
				0x103872C0, 0x1038736D, 0x1038DEA4, 0x10391D54,
				0x10398214, 0x103987EA, 0x103C0EB3, 0x103C44B5,
			};
			const auto stock_table = game::game_offset(0x10C48960);
			// Validate the entire patch set before changing any operand.
			for (const auto operand : operands)
			{
				std::uintptr_t actual{};
				std::memcpy(&actual, reinterpret_cast<const void*>(
					game::game_offset(operand)), sizeof(actual));
				if (actual != stock_table)
				{
					throw std::runtime_error("QoS sorted-material layout did not match PC 1.1");
				}
			}
			for (const auto operand : operands)
			{
				utils::hook::set<std::uintptr_t>(game::game_offset(operand),
					reinterpret_cast<std::uintptr_t>(sorted_materials));
			}
		}

		void extend_asset_pools()
		{
			// Pool targets are adapted from iAmThatMichael/T4M's
			// PatchT4MemoryLimits.cpp, but every address, stock count, and element
			// stride below is verified against QoS PC 1.1. DB_InitXAssetPools
			// (0x103DFA90) walks 38 entries and invokes the corresponding initializer
			// from 0x1055E898 with the pool pointer and count.
			static_assert(sizeof(game::Material) == 104);
			auto** const asset_pools = reinterpret_cast<void**>(game::game_offset(0x1055EA60));
			auto* const pool_sizes = reinterpret_cast<unsigned int*>(game::game_offset(0x1055E800));
			auto** const pool_initializers = reinterpret_cast<void**>(game::game_offset(0x1055E898));

			for (const auto& extension : asset_pool_extensions)
			{
				const auto type = static_cast<unsigned int>(extension.type);
				if (pool_sizes[type] != extension.stock_size
					|| asset_pools[type] != reinterpret_cast<void*>(game::game_offset(extension.stock_pool_address))
					|| pool_initializers[type] != reinterpret_cast<void*>(game::game_offset(extension.initializer_address)))
				{
					throw std::runtime_error("QoS asset-pool layout did not match PC 1.1");
				}

				asset_pools[type] = utils::memory::get_allocator()->allocate(
					extension.element_size * extension.extended_size);
				pool_sizes[type] = extension.extended_size;
			}
		}

		bool debug_xasset()
		{
			return utils::flags::has_flag("debug_xasset");
		}

		const char* asset_type_name(const int type)
		{
			static constexpr const char* names[]
			{
				"xmodelpieces", "physpreset", "physconstraints", "destructibledef",
				"xanim", "xmodel", "material", "techset", "image", "sound",
				"sndcurve", "col_map_sp", "col_map_mp", "com_map", "game_map_sp",
				"game_map_mp", "map_ents", "gfx_map", "lightdef", "ui_map",
				"font", "menulist", "menu", "localize", "weapon",
				"snddriverglobals", "fx", "impactfx", "aitype", "mptype",
				"character", "xmodelalias", "rawfile", "stringtable", "xmltree",
				"scene_animation", "cutscene", "custom_camera",
			};
			static_assert(sizeof(names) / sizeof(names[0]) == game::ASSET_TYPE_COUNT);

			return type >= 0 && type < game::ASSET_TYPE_COUNT ? names[type] : "unknown";
		}

		void __cdecl log_default_asset_creation(const int type, const char* name)
		{
			game::Com_Printf(16, "^3[fastfiles] creating default for missing asset type=%d(%s) name=%s\n",
				type, asset_type_name(type), name ? name : "<null>");
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

		// Temporary QoS PC 1.1 visibility probe; remove after converted world
		// submission is verified. Preserve the native thiscall and job contents.
		int __fastcall renderer_cell_job_stub(unsigned char* const job, void*)
		{
			InterlockedIncrement(&renderer_cell_jobs);
			InterlockedExchange(&renderer_last_surface_enabled, job[12]);
			InterlockedExchange(&renderer_last_cell,
				static_cast<LONG>(*reinterpret_cast<const std::uintptr_t*>(job)));
			InterlockedExchange(&renderer_last_view,
				static_cast<LONG>(*reinterpret_cast<const unsigned short*>(job + 10)));
			const auto original = reinterpret_cast<int(__thiscall*)(unsigned char*)>(
				renderer_cell_job_hook.get_original());
			return original(job);
		}

		void report_renderer_visibility_jobs()
		{
			static unsigned int samples = 0;
			if (samples >= 10 || !cg_initialized_address
				|| !*reinterpret_cast<const int*>(cg_initialized_address))
			{
				return;
			}
			++samples;
			game::Com_Printf(16,
				"^5[world-visibility] cellJobs=%ld bulkMarks=%ld leafLists=%ld lastCell=0x%08X view=%ld surfacesEnabled=%ld\n",
				InterlockedCompareExchange(&renderer_cell_jobs, 0, 0),
				InterlockedCompareExchange(&renderer_bulk_surface_marks, 0, 0),
				InterlockedCompareExchange(&renderer_leaf_surface_lists, 0, 0),
				static_cast<unsigned int>(InterlockedCompareExchange(&renderer_last_cell, 0, 0)),
				InterlockedCompareExchange(&renderer_last_view, 0, 0),
				InterlockedCompareExchange(&renderer_last_surface_enabled, 0, 0));
		}

		__declspec(naked) void renderer_surface_list_guard_stub()
		{
			__asm
			{
				// QoS PC 1.1 sub_1036E6F0 can retain a non-zero surface count
				// after its TLS list has been released during a Xenon map teardown.
				// During an active converted map, use the serialized GfxWorld DPVS
				// remap table when the PC-only TLS alias was never initialized.
				pushfd
				lock inc dword ptr[renderer_leaf_surface_lists]
				popfd
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
				game::Com_Printf(16, "^5[map-stage] DB_LoadXAsset %s type=%u(%s) token=0x%08X entry=%p\n",
					returning ? "returned" : "enter", type, asset_type_name(static_cast<int>(type)),
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
			bool multiplayer_map_requested = false;
			bool dlc_map_requested = false;
			for (int i = 0; zones && i < count; ++i)
			{
				if (zones[i].name && std::string_view(zones[i].name).starts_with("mp_"))
				{
					multiplayer_map_requested = true;
					dlc_map_requested |= dlc::requires_donor(utils::string::to_lower(zones[i].name));
					std::lock_guard lock(external_asset_log_mutex);
					logged_external_assets.clear();
				}
			}

			// Preflight before native DB_LoadXAssets can unload existing zones or queue IO.
			// QoS PC 1.1, 0x103E1CF0. Remove this adapter when native Xenon schemas exist.
			try
			{
				if (multiplayer_map_requested && !dlc_map_requested && common_xenon_attempted)
				{
					// Release only donor-owned assets. Freeing 0x11 would also
					// remove common_consolation and the native patch zones.
					game::XZoneInfo unload_donor{nullptr, 0, dlc::donor_free_flags};
					db_load_xassets_hook.invoke<int>(&unload_donor, 1, 0);
					game::DB_WaitXAssets.get()();
					common_xenon_loaded = false;
					common_xenon_attempted = false;
					game::Com_Printf(16, "^5[Xenon] Unloaded shared donor before stock map load\n");
				}
				// Ensure the required shared HUD assets finish linking before CG registers
				// map media, even if a map command races the deferred startup loader.
				if (multiplayer_map_requested && common_fastfiles_seen && !common_consolation_loaded)
				{
					if (!common_consolation_attempted)
					{
						if (!find_zone_file("common_consolation.ff"))
							throw std::runtime_error("required consolation/zone/common_consolation.ff is missing; reinstall the complete nightly archive");
						common_consolation_attempted = true;
						game::XZoneInfo hud_zone{"common_consolation", 0x11, 0};
						db_load_xassets_hook.invoke<int>(&hud_zone, 1, 0);
					}
					game::DB_WaitXAssets.get()();
					if (!common_consolation_loaded)
						throw std::runtime_error("required common_consolation HUD assets did not link");
				}
				bool unloads_zones = false;
				for (int i = 0; zones && i < count; ++i) unloads_zones |= zones[i].freeFlags != 0;
				for (int i = 0; zones && i < count; ++i)
				{
					if (!zones[i].name) continue;
					const auto source = find_zone_file(std::string(zones[i].name) + ".ff");
					if (source && xenon::prepare(*source, zones[i].name) && unloads_zones)
						throw std::runtime_error("Xenon UI conversion requires resident PC shader assets; use loadXenonZone without unloading zones");
				}

				if (dlc_map_requested && !common_xenon_attempted)
				{
					common_xenon_attempted = true;
					if (find_zone_file("common_xenon.ff"))
					{
						game::XZoneInfo donor_zone{"common_xenon", dlc::donor_alloc_flags, 0};
						game::Com_Printf(16, "^5[Xenon] Preloading shared PC render assets from common_xenon.ff\n");
						present_common_xenon_preload(donor_zone.name);
						const auto clear_preload_overlay = gsl::finally([]()
						{
							common_xenon_linking.store(false, std::memory_order_release);
							common_xenon_preload_complete.store(false, std::memory_order_release);
							common_xenon_preloading.store(false, std::memory_order_release);
						});
						common_xenon_linking.store(true, std::memory_order_release);
						db_load_xassets_hook.invoke<int>(&donor_zone, 1, 0);
						wait_for_common_xenon_with_ui();
						game::Com_Printf(16, common_xenon_loaded
							? "^5[Xenon] common_xenon.ff linked; continuing map load\n"
							: "^3[Xenon] common_xenon.ff submitted, but no linked assets were observed\n");
					}
					else
					{
						game::Com_Printf(16, "^3[Xenon] common_xenon.ff was not found; continuing without shared PC render assets\n");
					}
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

		__declspec(naked) void renderer_reflection_probe_stub()
		{
			__asm
			{
				// QoS PC 1.1 resolves the draw-surface reflection-probe index through
				// GfxWorld+0x10C. The Xenon source has two 16-byte records here, but
				// their cubemap format is not portable yet. A reduced world therefore
				// has no complete PC probe image table. Complete native worlds retain
				// the original lookup; absent tables and null image records take the
				// function's native no-binding branch.
				mov edx, dword ptr[10E29B3Ch]
				shrd eax, edi, 15h
				and eax, 0FFh
				shl eax, 4
				test edx, edx
				jz no_reflection_probe
				mov eax, dword ptr[eax + edx + 0Ch]
				test eax, eax
				jz no_reflection_probe
				jmp dword ptr[renderer_reflection_probe_continue]

			no_reflection_probe:
				jmp dword ptr[renderer_reflection_probe_skip]
			}
		}

		__declspec(naked) void renderer_reflection_probe_secondary_stub()
		{
			__asm
			{
				// A second draw-surface path performs the same GfxWorld+0x10C
				// reflection-probe lookup. It has different continuation and skip
				// addresses, so keep a distinct trampoline while applying the same
				// reduced-world guard.
				mov edx, dword ptr[10E29B3Ch]
				shrd eax, edi, 15h
				and eax, 0FFh
				shl eax, 4
				test edx, edx
				jz no_reflection_probe
				mov eax, dword ptr[eax + edx + 0Ch]
				test eax, eax
				jz no_reflection_probe
				jmp dword ptr[renderer_reflection_probe_secondary_continue]

			no_reflection_probe:
				jmp dword ptr[renderer_reflection_probe_secondary_skip]
			}
		}

		__declspec(naked) void renderer_surface_visibility_stub()
		{
			__asm
			{
				pushfd
				lock inc dword ptr[renderer_bulk_surface_marks]
				popfd
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

		game::XAssetEntry* db_link_xasset_entry_stub(game::XAssetEntry* entry, const int allow_override)
		{
			normalize_rawfile_name(entry);

			const auto* const incoming_name = get_asset_name(entry);
			const auto type = entry ? static_cast<int>(entry->asset.type) : -1;
			const auto incoming_zone_index = get_asset_zone_index(entry);
			const auto* const incoming_zone_name = get_zone_name(incoming_zone_index);
			relocate_portal_links(entry);
			const bool common_xenon_world_root = common_xenon_linking.load(std::memory_order_acquire)
				&& (type == game::ASSET_TYPE_CLIPMAP_MP || type == game::ASSET_TYPE_COMWORLD
					|| type == game::ASSET_TYPE_gameWORLD_MP || type == game::ASSET_TYPE_MAP_ENTS
					|| type == game::ASSET_TYPE_GFXWORLD);
			if (common_xenon_world_root)
			{
				// The current common_xenon probe is sourced from a complete native PC
				// map. Keep its render assets resident, but never register its world
				// roots: QoS permits only one GfxWorld and the requested map owns it.
				common_xenon_loaded = true;
				game::Com_Printf(16, "^5[Xenon] ignored common_xenon world root type=%d(%s) name=%s\n",
					type, asset_type_name(type), incoming_name);
				return entry;
			}
			// Temporary QoS PC 1.1 probe for the generated mp_canals zone. Remove
			// once native map asset linking and CM_LoadMap have been verified.
			const bool trace_map_asset = entry && (type == game::ASSET_TYPE_gameWORLD_MP
				|| type == game::ASSET_TYPE_CLIPMAP_MP || type == game::ASSET_TYPE_MAP_ENTS
				|| ((type == game::ASSET_TYPE_GFXWORLD || type == game::ASSET_TYPE_COMWORLD)
					&& std::strstr(incoming_name, "mp_canals") != nullptr));
			if (trace_map_asset)
			{
				game::Com_Printf(16, "^5[map-stage] link enter type=%d(%s) name=%s\n",
					type, asset_type_name(type), incoming_name);
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
					game::Com_Printf(16, "^5[fastfiles] resolving external asset type=%d(%s) name=%s\n",
						type, asset_type_name(type), incoming_name + 1);
				}
			}
			auto* const linked_entry = db_link_xasset_entry_hook.invoke<game::XAssetEntry*>(entry, allow_override);
			if (trace_map_asset)
			{
				game::Com_Printf(16, "^5[map-stage] link returned type=%d(%s) name=%s linked=%p\n",
					type, asset_type_name(type), incoming_name, linked_entry);
			}
			auto* const log_entry = linked_entry ? linked_entry : entry;
			if (log_entry)
			{
				const auto zone_index = get_asset_zone_index(log_entry);
				const auto* const zone_name = get_zone_name(zone_index);
				const auto* const linked_name = get_asset_name(log_entry);
				common_fastfiles_seen = common_fastfiles_seen || zone_name_equals(incoming_zone_name, "common_mp") || zone_name_equals(zone_name, "common_mp");
				patch_mp_loaded = patch_mp_loaded || zone_name_equals(incoming_zone_name, "patch_mp") || zone_name_equals(zone_name, "patch_mp");
				patch_consolation_loaded = patch_consolation_loaded || zone_name_equals(incoming_zone_name, "patch_consolation") || zone_name_equals(zone_name, "patch_consolation");
				common_consolation_loaded = common_consolation_loaded || zone_name_equals(incoming_zone_name, "common_consolation") || zone_name_equals(zone_name, "common_consolation");
				common_xenon_loaded = common_xenon_loaded || zone_name_equals(incoming_zone_name, "common_xenon") || zone_name_equals(zone_name, "common_xenon");

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
			if (common_fastfiles_seen && !common_consolation_attempted && !common_consolation_loaded)
			{
				common_consolation_attempted = true;
				if (zone_file_exists("common_consolation"))
					patch_zones.push_back(make_override_zone("common_consolation"));
				else
					game::Com_Printf(16, "^1Required client fastfile missing: consolation/zone/common_consolation.ff\n");
			}
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

			const auto common_consolation_expected = has_zone(patch_zones.data(), static_cast<int>(patch_zones.size()), "common_consolation");
			if ((!patch_mp_expected || patch_mp_loaded) && (!patch_consolation_expected || patch_consolation_loaded)
				&& (!common_consolation_expected || common_consolation_loaded))
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
			extend_sorted_material_storage();
			extend_asset_pools();
			scheduler::loop(draw_common_xenon_preload, scheduler::pipeline::renderer);
			gfx_world_pointer_address = game::game_offset(0x10C4A354);
			cg_initialized_address = game::game_offset(0x129FE8E4);
			// PC sub_103ABCF0 dispatches this job to sub_1036E8B0 with
			// its pointer in ECX; byte +12 enables surface visibility marking.
			renderer_cell_job_hook.create(game::game_offset(0x1036E8B0), renderer_cell_job_stub);
			scheduler::loop(report_renderer_visibility_jobs, scheduler::main, 2000ms);
			// sub_103A4840 assumes every cell probe index has a matching world
			// origin array. Generated reduced worlds do not serialize that PC-only
			// array yet, so preserve native lookup only when the array exists.
			reflection_probe_nearest_hook.create(game::game_offset(0x103A4840), reflection_probe_nearest_stub);
			reflection_probe_nearest_original = reflection_probe_nearest_hook.get_original();
			// Runtime crash evidence: 0x1037E7FF read 0x0000000C after resolving a
			// draw surface with reflectionProbeIndex 0 through the absent reduced
			// world's GfxWorld+268 probe table. The Xbox loader confirms a 16-byte
			// record with the cubemap image pointer at +12; skip only that unavailable
			// cubemap binding until its Xenos format is converted.
			renderer_reflection_probe_continue = game::game_offset(0x1037E803);
			renderer_reflection_probe_skip = game::game_offset(0x1037E819);
			utils::hook::nop(game::game_offset(0x1037E7ED), 22);
			utils::hook::jump(game::game_offset(0x1037E7ED), renderer_reflection_probe_stub);
			// Runtime crash evidence: 0x1038060D is the corresponding lookup in
			// the second draw-surface path. Guard its absent table/image record too.
			renderer_reflection_probe_secondary_continue = game::game_offset(0x10380611);
			renderer_reflection_probe_secondary_skip = game::game_offset(0x10380629);
			utils::hook::nop(game::game_offset(0x103805FB), 22);
			utils::hook::jump(game::game_offset(0x103805FB), renderer_reflection_probe_secondary_stub);
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
			common_xenon_linking.store(false, std::memory_order_release);
			common_xenon_preload_complete.store(false, std::memory_order_release);
			common_xenon_preloading.store(false, std::memory_order_release);
			xenon::clear();
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
