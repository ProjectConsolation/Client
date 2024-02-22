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

		bool IsNoBorder()
		{
			if (dvars::r_noborder && dvars::r_noborder->current.enabled)
			{
				return true;
			}

			return false;
		}

		void StyleHookStub()
		{
			const static uint32_t retn_pt = game::game_offset(0x103BE2B9); //updated for QoS
			__asm
			{
				call	IsNoBorder;
				test	al, al

					jz		SetBorder

					mov		ebp, WS_VISIBLE | WS_POPUP;
				jmp		retn_pt;

			SetBorder:
				mov		ebp, WS_VISIBLE | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX;
				jmp		retn_pt;
			}
		}

		void vid_xypos_stub()
		{ //IW3SP
			const static uint32_t retn_addr = game::game_offset(0x103BF068);
			__asm
			{
				mov[esi + 10h], eax;	    // overwritten op (wndParms->y)
				mov		dword ptr[esi], 0;	// overwritten op

				pushad;
				call	IsNoBorder;
				test	al, al;
				jnz		NO_BORDER;

				popad;
				jmp		retn_addr;


			NO_BORDER:
				popad;
				xor eax, eax;			// clear eax
				mov[esi + 0Ch], eax;	// set wndParms->x to 0 (4 byte)
				mov[esi + 10h], eax;	// set wndParms->y to 0 (4 byte)
				jmp		retn_addr;
			}
		}

		HWND __stdcall create_window_ex_stub(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
		{
			if (!strcmp(class_name, "JB_MP"))
			{
				ex_style = 0;
				//style = WS_POPUP;
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

			// Do not use vid_xpos / vid_ypos when r_noborder is enabled - IW3SP
			//utils::hook::nop(game::game_offset(0x103BF060), 9);
			//utils::hook::set(game::game_offset(0x103BF060), vid_xypos_stub);  - IW3SP (lazy) test port

			// Main window border
			//utils::hook::set(game::game_offset(0x103BE2B5), StyleHookStub); - IW3SP (lazy) test port
#endif

			// un-cap fps
			utils::hook::set<uint8_t>(game::game_offset(0x103F696A), 0x00);

			// fix LOD distance at higher FOVs
			// FLOAT: base+0x1054688 + 10 (int value) --> set to 0
			//utils::hook::set<uint8_t>(game::game_offset(0x114A4420), 0x00); //check if this works. it doesnt lolo

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
		}
	};
}

REGISTER_COMPONENT(patches::component)
