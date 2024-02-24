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

		std::time_t now = std::time(nullptr);
		std::tm tm = *std::localtime(&now);
		char timeBuffer[80];
		const auto curTime = std::strftime(timeBuffer, sizeof(timeBuffer), "%a %b %d %H:%M:%S %Y", &tm);

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

	//IW3SP-MOD's code, thanks :)
	void ScrPlace_ApplyRect(const game::qos::ScreenPlacement* ScrPlace, float* x, float* y, float* w, float* h, int horzAlign, int vertAlign)
	{
		float v7;
		float v8;
		float v9;
		float v10;

		switch (horzAlign)
		{
		case 7:
			v7 = *x * ScrPlace->scaleVirtualToReal[0];
			v8 = (float)(ScrPlace->realViewableMin[0] + ScrPlace->realViewableMax[0]) * 0.5;
			*x = v7 + v8;
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		case 6:
			*x = *x * ScrPlace->scaleRealToVirtual[0];
			*w = *w * ScrPlace->scaleRealToVirtual[0];
			break;
		case 5:
			break;
		case 4:
			*x = *x * ScrPlace->scaleVirtualToFull[0];
			*w = *w * ScrPlace->scaleVirtualToFull[0];
			break;
		case 3:
			*x = (float)(*x * ScrPlace->scaleVirtualToReal[0]) + ScrPlace->realViewableMax[0];
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		case 2:
			v7 = *x * ScrPlace->scaleVirtualToReal[0];
			v8 = 0.5 * ScrPlace->realViewportSize[0];
			*x = v7 + v8;
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		case 1:
			*x = (float)(*x * ScrPlace->scaleVirtualToReal[0]) + ScrPlace->realViewableMin[0];
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		default:
			*x = (float)(*x * ScrPlace->scaleVirtualToReal[0]) + ScrPlace->subScreenLeft;
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		}

		switch (vertAlign)
		{
		case 7:
			v9 = *y * ScrPlace->scaleVirtualToReal[1];
			v10 = (float)(ScrPlace->realViewableMin[1] + ScrPlace->realViewableMax[1]) * 0.5;
			*y = v9 + v10;
			*h = *h * ScrPlace->scaleVirtualToReal[1];
		case 6:
			*y = *y * ScrPlace->scaleRealToVirtual[1];
			*h = *h * ScrPlace->scaleRealToVirtual[1];
			break;
		case 5:
			return;
		case 4:
			*y = *y * ScrPlace->scaleVirtualToFull[1];
			*h = *h * ScrPlace->scaleVirtualToFull[1];
			break;
		case 3:
			*y = (float)(*y * ScrPlace->scaleVirtualToReal[1]) + ScrPlace->realViewableMax[1];
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		case 2:
			v9 = *y * ScrPlace->scaleVirtualToReal[1];
			v10 = 0.5 * ScrPlace->realViewportSize[1];
			*y = v9 + v10;
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		case 1:
			*y = (float)(*y * ScrPlace->scaleVirtualToReal[1]) + ScrPlace->realViewableMin[1];
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		default:
			*y = *y * ScrPlace->scaleVirtualToReal[1];
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		}
	}

	//xoxor3d's code, thanks :)
	void R_AddCmdDrawTextASM(const char* text, int max_chars, void* font, float x, float y, float x_scale, float y_scale, float rotation, const float* color, int style)
	{
		const static uint32_t R_AddCmdDrawText_func = game::game_offset(0x103C02B0); //AddBaseDrawTextCmd, no idea if it works :shrug:
		__asm
		{
			push	style;
			sub     esp, 14h;

			fld		rotation;
			fstp[esp + 10h];

			fld		y_scale;
			fstp[esp + 0Ch];

			fld		x_scale;
			fstp[esp + 8];

			fld		y;
			fstp[esp + 4];

			fld		x;
			fstp[esp];

			push	font;
			push	max_chars;
			push	text;
			mov		ecx, [color];

			call	R_AddCmdDrawText_func;
			add		esp, 24h;
		}
	}

	//IW3SP-MOD's code, thanks :)
	void UI_DrawText(const game::qos::ScreenPlacement* ScrPlace, const char* text, int maxChars, game::qos::Font_s* font, float ix, float iy, int horzAlign, int vertAlign, float scale, const float* color, int style)
	{
		float xScale = scale * 48.0f / static_cast<float>(font->pixelHeight);
		float yScale = xScale;

		ScrPlace_ApplyRect(ScrPlace, &ix, &iy, &xScale, &yScale, horzAlign, vertAlign);
		int x = floor(ix + 0.5);
		int y = floor(iy + 0.5);
		R_AddCmdDrawTextASM(text, maxChars, font, x, y, xScale, yScale, 0.0, color, style);
	}

	// IW3SP-MOD, thanks again, though this one was simple, lul
	game::qos::ScreenPlacement* ScrPlace_GetFullPlacement()
	{
		return game::scrPlaceFull;
	}

	const char* drawVersion()
	{
		//auto* const scrPlace = ScrPlace_GetFullPlacement();

		//const float color[4] = { 0.0f, 0.80f, 0.0f, 0.69f };
		//game::qos::Font_s* fontHandle = game::R_RegisterFont("fonts/objectiveFont", sizeof("fonts/objectiveFont"));

		//float fontScale = (0.2f * 48.0f / static_cast<float>(fontHandle->pixelHeight)) * game::scrPlace->scaleVirtualToReal[0];
		//float xPos = 50.f * game::scrPlace->scaleVirtualToReal[0];
		//float yPos = 18.f * game::scrPlace->scaleVirtualToReal[1];
		//UI_DrawText(scrPlace, utils::string::va("%d %s %s", 549, "CONSOLATION", buffer), 128, fontHandle, xPos, yPos, 0, 0, fontScale, color, 0);
		const char* out = utils::string::va("%d %s %s", 549, "CONSOLATION", timeBuffer);
		return out;
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
			utils::hook::nop(game::game_offset(0x103F696A), 0x00);

			// branding
			//utils::hook::set<const char*>(game::game_offset(0x12BBAEA0), utils::string::va("%d %s %s", 549, "CONSOLATION", curTime)); //doesnt work?
			//utils::hook::set<const char*>(game::game_offset(0x104CAD58), "QOS-CSLT"); // "007GUEST" Branding
			//utils::hook::set<const char*>(game::game_offset(0x104CAD3C), utils::string::va("%s", curTime)); //Date + Time

			// fix LOD distance at higher FOVs
			// FLOAT: base+0x1054688 + 10 (int value) --> set to 0
			// in mem this would be 00 00 @ game_offset+14A4422 & 23

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
