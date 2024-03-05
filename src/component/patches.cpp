#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <unordered_set>

#define FORCE_BORDERLESS // still needs a few things fixed - 3rd of march, does it still?
//#define XLIVELESS

namespace patches
{
	namespace
	{
		game::dvar_s* r_borderless = nullptr;

		int ret_zero()
		{
			return 0;
		}

		int ret_one(DWORD*, int)
		{
			return 1;
		}

		HWND __stdcall create_window_ex_stub(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
		{
			if (!strcmp(class_name, "JB_MP"))
			{
				window_name = "Project Consolation: Multiplayer";
			}
			return CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, inst, param);
		}

		utils::hook::detour link_xasset_entry_hook;
		game::XAssetEntry* link_xasset_entry_stub(game::XAssetEntry* entry, int override)
		{
			if (entry->asset.type == game::ASSET_TYPE_GFXWORLD)
			{
				//const auto troll = entry->asset.header.gfxWorld;
				//printf("");
			}

			return link_xasset_entry_hook.invoke<game::XAssetEntry*>(entry, override);
		}

		template <typename T>
		T* find_dvar(std::unordered_map<std::string, T>& map, const std::string& name)
		{
			auto i = map.find(name);
			if (i != map.end())
			{
				return &i->second;
			}

			return nullptr;
		}

		bool find_dvar(std::unordered_set<std::string>& set, const std::string& name)
		{
			return set.find(name) != set.end();
		}

		utils::hook::detour dvar_registernew_hook;
		game::dvar_s* Dvar_RegisterNew_Stub(const char* dvarName, game::DvarType type, unsigned short flags, char* desc, int unk, game::DvarValue value, game::DvarLimits domain)
		{
			if (type == game::DVAR_TYPE_INT)
			{
				auto* var = find_dvar(dvars::overrides::register_int_overrides, dvarName);
				if (var)
				{
					value.integer = var->value;
					domain.integer.max = var->max;
					domain.integer.min = var->min;
					flags = var->flags;
				}
			}

			return dvar_registernew_hook.invoke<game::dvar_s*>(dvarName, type, flags, desc, unk, value, domain);
		}

		utils::hook::detour BG_GetPlayerJumpHeight_hook;
		float BG_GetPlayerJumpHeight_stub(int a1)
		{
			auto jump_height = game::Dvar_FindVar("jump_height");

			if (!jump_height)
				return BG_GetPlayerJumpHeight_hook.invoke<float>(a1);

			return jump_height->current.value;
		}

		utils::hook::detour BG_GetPlayerSpeed_hook;
		float BG_GetPlayerSpeed_stub(int a1)
		{
			auto g_speed = game::Dvar_FindVar("g_speed");

			if (!g_speed)
				return BG_GetPlayerSpeed_hook.invoke<float>(a1);

			return g_speed->current.value;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
#ifdef FORCE_BORDERLESS
			// force fullscreen to always be false
			utils::hook::nop(game::game_offset(0x103BE1A2), 2);

			// change window style to 0x90000000
			utils::hook::set<std::uint8_t>(game::game_offset(0x103BD70C) + 3, 0x00);
			utils::hook::set<std::uint8_t>(game::game_offset(0x103BD70C) + 4, 0x90);

			scheduler::once([]()
				{
					r_borderless = game::Dvar_RegisterBool("r_borderless", true, 0, "Remove the windows border when in windowed mode.");
				}, scheduler::main);

#endif
			// branding - intercept import for CreateWindowExA to change window title
			utils::hook::set(game::game_offset(0x1047627C), create_window_ex_stub);

			// un-cap fps
			utils::hook::nop(game::game_offset(0x103F696A), 0x01);

			// nop call to Com_Printf for "SCALEFORM: %s" messages
			utils::hook::nop(game::game_offset(0x1000230F), 0x05); // TODO: Dvar toggle? Could be useful info
			utils::hook::nop(game::game_offset(0x102E1284), 0x05);
			// nop above call to Com_Printf for "unknown UI script %s in block:\n%s\n"

			// LOD Scaling fix: Probably best way is to re-register the dvar and accept 0 as minimum value
			// 0x1054688 + 10 (dvar pointer) --> set to 0 (r_lodScale)

			// various hooks to return dvar functionality, thanks to Liam
			//BG_GetPlayerJumpHeight_hook.create(game::game_offset(0x101E6900), BG_GetPlayerJumpHeight_stub);
			//BG_GetPlayerSpeed_hook.create(game::game_offset(0x101E6930), BG_GetPlayerSpeed_stub);

#ifdef DEBUG
			// hook linkxassetentry to debug stuff
			link_xasset_entry_hook.create(game::game_offset(0x103E0640), link_xasset_entry_stub);
#endif

			// support xliveless emulator
#ifdef XLIVELESS
			// bypass playlist + stats
			utils::hook::jump(game::game_offset(0x10240B30), ret_one);
			utils::hook::jump(game::game_offset(0x10240A30), ret_one);

			// allow map loading
			utils::hook::nop(game::game_offset(0x102489A1), 5);
#endif
			scheduler::once([]()
			{
				dvars::overrides::register_int("g_speed", 500, 0, 1000, game::dvar_flags::saved);
				dvar_registernew_hook.create(game::Dvar_RegisterNew, Dvar_RegisterNew_Stub);
			}, scheduler::main);

		}
	};
}

REGISTER_COMPONENT(patches::component)
