#pragma once

#define WEAK __declspec(selectany)

namespace game
{
	WEAK symbol<void(int channel, const char* fmt, ...)> Com_Printf{ game_offset(0x103F6400) };
	WEAK symbol<void(int errorParmCode, const char* message, ...)> Com_Error{ game::game_offset(0x1) };
	WEAK symbol<void(int channel, const char* fmt)> Conbuf_AppendText{ game_offset(0x102C3CE0) };

	WEAK symbol<qos::XAssetHeader(qos::XAssetType type, const char* name)> DB_FindXAssetHeader{ game_offset(0x103E2260) };

	WEAK symbol<qos::dvar_s*(const char* dvarName)> Dvar_FindVar{ game_offset(0x103BA28) };
	WEAK symbol<void(qos::dvar_s* dvarName, const char* value)> Dvar_SetString{ game_offset(0x10277E60) };
	//WEAK symbol<qos::dvar_s*(const char* dvar_name, game::qos::dvar_type flags, std::int32_t default_value)> Dvar_RegisterBool{ game_offset(0x10278E60) };

	WEAK symbol<game::qos::Font_s* (const char* fontName, int imageTrack)> R_RegisterFont{ game::game_offset(0x10199E74) };

	WEAK symbol<int(const char* name)> Scr_LoadScript_{ game_offset(0x1022E7C0) };
	
	WEAK symbol<void()> Sys_ShowConsole{ game_offset(0x102C4230) };

	// Variables
	WEAK symbol<DWORD> command_id{ game_offset(0x10752C70) };
	WEAK symbol<DWORD> cmd_argc{ game_offset(0x10752CB4) };
	WEAK symbol<char**> cmd_argv{ game_offset(0x10752CD4) };
	WEAK symbol<qos::cmd_function_s*> cmd_functions{ game_offset(0x10752CF8) };
	WEAK symbol<qos::scrMemTreePub_t> scrMemTreePub{ game_offset(0x116357CC) };
	WEAK symbol<qos::ScreenPlacement> scrPlace{ game_offset(0x1127BAA0) };
	WEAK symbol<qos::ScreenPlacement> scrPlaceFull{ game_offset(0x1127BA50) };

	WEAK symbol<unsigned short> db_hashTable{ game_offset(0x1082ED60) };
	WEAK symbol<qos::XAssetEntryPoolEntry> g_assetEntryPool{ game_offset(0x108CB5C0) };

	WEAK symbol<unsigned int> scr_numParams{ game_offset(0x117384A4) };

	WEAK symbol<qos::scrVmPub_t> scrVmPub{ game_offset(0x11738488) };
}
