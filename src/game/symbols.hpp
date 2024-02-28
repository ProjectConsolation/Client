#pragma once

#define WEAK __declspec(selectany)

namespace game
{
	WEAK symbol<void(int channel, const char* fmt, ...)> Com_Printf{ game_offset(0x103F6400) };
	WEAK symbol<void(int channel, const char* fmt)> Conbuf_AppendText{ game_offset(0x102C3CE0) };

	WEAK symbol<XAssetHeader (XAssetType type, const char* name)> DB_FindXAssetHeader{ game_offset(0x103E2260) };

	WEAK symbol<void()> Sys_ShowConsole{ game_offset(0x102C4230) };

	WEAK symbol<DWORD> command_id{ game_offset(0x10752C70) };
	WEAK symbol<DWORD> cmd_argc{ game_offset(0x10752CB4) };
	WEAK symbol<char**> cmd_argv{ game_offset(0x10752CD4) };
	WEAK symbol<cmd_function_s*> cmd_functions{ game_offset(0x10752CF8) };
	WEAK symbol<scrMemTreePub_t> scrMemTreePub{ game_offset(0x116357CC) };

	WEAK symbol<unsigned short> db_hashTable{ game_offset(0x1082ED60) };
	WEAK symbol<XAssetEntryPoolEntry> g_assetEntryPool{ game_offset(0x108CB5C0) };
	WEAK symbol<void(float x, float y, float width, float height, float s0, float t0, float s1, float t1,
		float* color, Material* material, int unkown)> R_AddCmdDrawStretchPic{ game_offset(0x103C0820) };

	WEAK symbol<void(const char* text, int maxChars, Font_s* font, double x, double y, double xScale, double yScale, double rotation, float* color, int style)> 
		R_AddCmdDrawText{ game_offset(0x103C02B0) };

	WEAK symbol<int(const char* text, int maxChars, Font_s* font)> R_TextWidth{ game_offset(0x1037CFA0) };
	WEAK symbol<const char* (dvar_s* dvar, DvarValue value)> Dvar_ValueToString{ game_offset(0x10274F80) };

	WEAK symbol <dvar_s*(const char* dvarName, DvarType type, unsigned short flags, DvarValue value, DvarLimits domain)>
		Dvar_RegisterNew{ game_offset(0x10276980) };

	WEAK symbol<int(char* dvar)> generateHashValue{ game_offset(0x10275260) };
	WEAK symbol<dvar_s*> dvarHashTable{game_offset(0x1149FCE0)};

	WEAK symbol<int> keyCatchers{ game_offset(0x11A7AB84) };
	WEAK symbol<PlayerKeyState> playerKeys{ game_offset(0x11263618) };
	WEAK symbol<ScreenPlacement> scrPlaceView{ game_offset(0x1127BA50) };
	WEAK symbol<void()> Con_DrawConsole{game_offset(0x10311F70)};
}
