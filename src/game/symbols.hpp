#pragma once

#define WEAK __declspec(selectany)

namespace game
{
	WEAK symbol<void(int channel, const char* fmt, ...)> Com_Printf{ game_offset(0x103F6400) };
	WEAK symbol<void(int a1, int a2, int a3, char* Format, ...)> Com_Error{ game_offset(0x103F77B0) };
	WEAK symbol<void(int channel, const char* fmt)> Conbuf_AppendText{ game_offset(0x102C3CE0) };
	WEAK symbol<void()> Con_DrawConsole{game_offset(0x10311F70)};

	WEAK symbol<void(XAssetType type, void(*)(XAssetHeader, void*), const void* userdata, bool overrides)> DB_EnumXAssets_FastFile{ game_offset(0x103DFCA0) };
	WEAK symbol<XAssetHeader (XAssetType type, const char* name)> DB_FindXAssetHeader{ game_offset(0x103E2260) };
	WEAK symbol<XAssetHeader(XAssetType type, const char* name, int create_default)> DB_FindXAssetHeader_Internal{ game_offset(0x103E1EE0) };
	WEAK symbol <dvar_s* (const char* dvarName, DvarType type, unsigned short flags, const char* description, int unk, DvarValue value, DvarLimits domain)>
		Dvar_RegisterNew{ game_offset(0x10276980) };

	//IW3SP-MOD
	typedef const char* (*DB_XAssetGetNameHandler_t)(game::XAssetHeader* asset);
	extern DB_XAssetGetNameHandler_t* DB_XAssetGetNameHandler;
	//

	WEAK symbol<void(float x, float y, float width, float height, float s0, float t0, float s1, float t1,
		float* color, Material* material, int unknown)> R_AddCmdDrawStretchPic{ game_offset(0x103C0820) };
	WEAK symbol<void(const char* text, int maxChars, Font_s* font, float x, float y, float xScale, float yScale, float rotation, const float* color, int style)>
		R_AddCmdDrawText{ game_offset(0x103C02B0) };
	WEAK symbol<int(const char* text, int maxChars, Font_s* font)> R_TextWidth{ game_offset(0x1037CFA0) };

	WEAK symbol<int(const char* name)> Scr_LoadScript_{ game_offset(0x1022E7C0) };
	WEAK symbol<void()> Sys_ShowConsole{ game_offset(0x102C4230) };

	WEAK symbol<char*(char* result, int a2)> sub_1022D690{ game_offset(0x1022D690) };

	WEAK symbol<DWORD> command_id{ game_offset(0x10752C70) };
	WEAK symbol<DWORD> cmd_argc{ game_offset(0x10752CB4) };
	WEAK symbol<char**> cmd_argv{ game_offset(0x10752CD4) };
	WEAK symbol<cmd_function_s*> cmd_functions{ game_offset(0x10752CF8) };
	WEAK symbol<scrMemTreePub_t> scrMemTreePub{ game_offset(0x116357CC) };
	WEAK symbol<unsigned short> db_hashTable{ game_offset(0x1082ED60) };
	WEAK symbol<XAssetEntryPoolEntry> g_assetEntryPool{ game_offset(0x108CB5C0) };
	WEAK symbol<int(char* dvar)> generateHashValue{ game_offset(0x10275260) };
	WEAK symbol<dvar_s*> dvarHashTable{game_offset(0x1149FCE0)};
	WEAK symbol<const char*> g_assetNames{ game_offset(0x1055E3D8) };
	WEAK symbol<int> keyCatchers{ game_offset(0x11A7AB84) };
	WEAK symbol<PlayerKeyState> playerKeys{ game_offset(0x11263618) };
	WEAK symbol<ScreenPlacement> scrPlaceView{ game_offset(0x1127BA50) };
	WEAK symbol<unsigned int> scr_numParams{ game_offset(0x117384A4) };
	WEAK symbol<scrVmPub_t> scrVmPub{ game_offset(0x11738488) };
	WEAK symbol<int> dvarCount{ game_offset(0x1149FCC8) };
	WEAK symbol<dvar_s*> sortedDvars{ game_offset(0x1149FCD4) };
	WEAK symbol<dvar_s> r_LodScale{ game_offset(0x11054688) };
}
