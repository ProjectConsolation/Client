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

			if (type == game::DVAR_TYPE_BOOL)
			{
				auto* var = find_dvar(dvars::overrides::register_bool_overrides, dvarName);
				if (var)
				{

					value.enabled = var->value;
					flags = var->flags;
				}
			}

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

			if (type == game::DVAR_TYPE_FLOAT)
			{
				auto* var = find_dvar(dvars::overrides::register_float_overrides, dvarName);
				if (var)
				{
					value.value = var->value;
					domain.value.max = var->max;
					domain.value.min = var->min;
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
		int BG_GetPlayerSpeed_stub(int a1)
		{
			auto g_speed = game::Dvar_FindVar("g_speed");

			if (!g_speed)
				return BG_GetPlayerSpeed_hook.invoke<int>(a1);

			return g_speed->current.integer;
		}


		float __cdecl Jump_GetLandFactor(DWORD* ps)
		{
			__int64 v1; // r10
			double v2; // fp1

			auto jump_slowdownEnable = game::Dvar_FindVar("jump_slowdownEnable");
			if (jump_slowdownEnable->current.enabled)
			{
				if (*(DWORD*)(ps + 24) < 1700)
				{
					v1 = *(DWORD*)(ps + 24);
					v2 = (float)((float)((float)v1 * (float)0.00088235294) + (float)1.0);
				}
				else
				{
					v2 = 2.5;
				}
			}
			else
			{
				v2 = 1.0;
			}
			return *((float*)&v2 + 1);
		}

		utils::hook::detour Jump_Start_hook;
		int Jump_Start_stub(int unused, int unused2, DWORD* pml_t)
		{
			DWORD* pmove_t{};
			float jump_height = game::Dvar_FindVar("jump_height")->current.value;

			_asm
			{
				mov  edi, DWORD PTR[edi]; edi = *edi
				mov  DWORD PTR[pmove_t], edi
			}

			auto v3 = *pmove_t;
			auto gravity = *(int*)(*pmove_t + 0x68);
			auto calculatedGravity = (double)gravity * (jump_height + jump_height);

			if ((*(DWORD*)(*pmove_t + 12) & 0x4000) != 0 && *(DWORD*)(v3 + 24) <= 1800)
			{
				auto landFactor = Jump_GetLandFactor(pmove_t);
				calculatedGravity = (float)((float)calculatedGravity / (float)landFactor);
			}

			pml_t[12] = 0;
			pml_t[13] = 0;
			pml_t[11] = 0;

			auto zOrigin = *(float*)(v3 + 40);
			*(DWORD*)(v3 + 128) = 1023; // groundEntityNum
			auto serverTime = pmove_t[1];
			*(float*)(v3 + 140) = zOrigin;

			*(DWORD*)(v3 + 136) = serverTime;
			auto v9 = sqrt(calculatedGravity);
			auto v11 = *(DWORD*)(v3 + 12) & 0xFFFFFE7F | 0x4000;
			*(float*)(v3 + 52) = v9;
			*(DWORD*)(v3 + 12) = v11;
			*(DWORD*)(v3 + 24) = 0;
			*(DWORD*)(v3 + 3900) = 0;

			auto v13 = game::Dvar_FindVar("jump_spreadAdd")->current.value;
			auto v14 = *(float*)(v3 + 4340) + v13;
			*(float*)(v3 + 4340) = v14;
			if (v14 > 255.0)
				*(DWORD*)(v3 + 4340) = 255.0;

			return v13;
		}

		utils::hook::detour dvar_registerlodscale_hook;
		game::dvar_s* dvar_registerlodscale_stub()
		{
			return dvars::Dvar_RegisterFloat("r_lodScale", "Scale the level of detail distance (larger reduces detail)", 0, 0, 3, game::dvar_flags::saved);
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

			/*scheduler::once([]()
				{
					r_borderless = game::Dvar_RegisterBool("r_borderless", true, 0, "Remove the windows border when in windowed mode.");
				}, scheduler::main);*/

#endif
			// branding - intercept import for CreateWindowExA to change window title
			utils::hook::set(game::game_offset(0x1047627C), create_window_ex_stub);

			// un-cap fps
			utils::hook::nop(game::game_offset(0x103F696A), 0x01);

			// nop call to Com_Printf for "SCALEFORM: %s" messages
			utils::hook::nop(game::game_offset(0x1000230F), 0x05); // TODO: Dvar toggle? Could be useful info
			utils::hook::nop(game::game_offset(0x102E1284), 0x05);
			// nop above call to Com_Printf for "unknown UI script %s in block:\n%s\n"

			// various hooks to return dvar functionality, thanks to Liam
			BG_GetPlayerJumpHeight_hook.create(game::game_offset(0x101E6900), BG_GetPlayerJumpHeight_stub);
			BG_GetPlayerSpeed_hook.create(game::game_offset(0x101E6930), BG_GetPlayerSpeed_stub);

			Jump_Start_hook.create(game::game_offset(0x101DB390), Jump_Start_stub);

			// support xliveless emulator
#ifdef XLIVELESS
			// bypass playlist + stats
			utils::hook::jump(game::game_offset(0x10240B30), ret_one);
			utils::hook::jump(game::game_offset(0x10240A30), ret_one);

			// allow map loading
			utils::hook::nop(game::game_offset(0x102489A1), 5);
#endif

			dvars::Dvar_RegisterBool("g_debugVelocity", 0, "Print velocity debug information to console", game::dvar_flags::none); //testing stuff
			dvars::overrides::register_bool("sv_cheats", 1, game::dvar_flags::none); //testing stuff
			dvars::overrides::register_int("g_speed", 210, 0, 1000, game::dvar_flags::saved);
			
			dvar_registernew_hook.create(game::Dvar_RegisterNew, Dvar_RegisterNew_Stub);

			scheduler::once([]
			{
				utils::hook::nop(game::game_offset(0x103AF41F), 5);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054688)) = dvars::Dvar_RegisterFloat("r_lodScale", "Scale the level of detail distance (larger reduces detail)", 0, 0, 3, game::dvar_flags::saved);
				dvars::overrides::register_float("ui_smallFont", 0.0, 0, 1, game::dvar_flags::saved);
				dvars::overrides::register_float("ui_bigFont", 0.0, 0, 1, game::dvar_flags::saved);
				dvars::overrides::register_float("ui_extraBigFont", 0.0, 0, 1, game::dvar_flags::saved);
				dvars::overrides::register_float("cg_overheadNamesSize", 0.5, 0, 1, game::dvar_flags::saved);
				dvars::overrides::register_float("jump_height", 48.0, 0, 99999, game::dvar_flags::saved); //adjusted to 48 to allow cod4-like jump onto ledges
				dvars::overrides::register_float("r_lodScale", 0, 0, 3, game::dvar_flags::saved); 
			}, scheduler::main);
		}
	};
}

REGISTER_COMPONENT(patches::component)
