#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"

#include "game/game.hpp"

#include <mmeapi.h>

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <component/dvars.hpp>

#define FORCE_BORDERLESS // still needs a few things fixed
#define XLIVELESS

namespace patches
{
	namespace
	{
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
				ex_style = 0;
				style = WS_POPUP;
				x = 0;
				y = 0;
				window_name = "Project Consolation: Multiplayer";
			}
			return CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, inst, param);
		}

		utils::hook::detour link_xasset_entry_hook;
		game::qos::XAssetEntry* link_xasset_entry_stub(game::qos::XAssetEntry* entry, int override)
		{
			if (entry->asset.type == game::qos::ASSET_TYPE_GFXWORLD)
			{
				//const auto troll = entry->asset.header.gfxWorld;
				//printf("");
			}

			return link_xasset_entry_hook.invoke<game::qos::XAssetEntry*>(entry, override);
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

			// return 0 for x & y pos
			//utils::hook::call(game::game_offset(0x103BE2AD), ret_zero); // vid_xpos
			//utils::hook::call(game::game_offset(0x103BE2DF), ret_zero); // vid_ypos

			// intercept import for CreateWindowExA to change window stuff
		    utils::hook::set(game::game_offset(0x1047627C), create_window_ex_stub);
#endif

			// un-cap fps
			utils::hook::nop(game::game_offset(0x103F696A), 0x01);

			// nop call to Com_Printf for "SCALEFORM: %s" messages
			utils::hook::nop(game::game_offset(0x1000230F), 0x05); // TODO: Make this a Dvar maybe? Could be useful info
			utils::hook::nop(game::game_offset(0x102E1284), 0x05); // 
			// nop above call to Com_Printf for "unknown UI script %s in block:\n%s\n"

			// LOD Scaling fix
			// Was going to do this via mem editing, but i cant figure out how, this could be a dirty patch?
			// FLOAT: base+0x1054688 + 10 (dvar pointer) (int value) --> set to 0
			
			//game::Cbuf_AddText(0, "r_lodScale 0");

#ifdef DEBUG
			// hook linkxassetentry to debug stuff
			link_xasset_entry_hook.create(game::game_offset(0x103E0640), link_xasset_entry_stub);
#endif

// support xliveless emulator
#ifdef XLIVELESS
			// bypass playlist + stats
			//utils::hook::jump(game::game_offset(0x10240B30), ret_one);
			//utils::hook::jump(game::game_offset(0x10240A30), ret_one);

			// allow map loading
			utils::hook::nop(game::game_offset(0x102489A1), 5);
#endif
		}
	};
}

REGISTER_COMPONENT(patches::component)
