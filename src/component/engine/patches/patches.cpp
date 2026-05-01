#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <unordered_set>

#ifndef VERSION_BUILD
#define VERSION_BUILD "0"
#endif

//#define XLIVELESS

namespace patches
{
	void enforce_ads_sprint_interrupt(game::usercmd_t* cmd)
	{
		if (!cmd)
		{
			return;
		}

		if ((cmd->buttons & game::BUTTON_ADS) != 0)
		{
			cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons & ~game::BUTTON_SPRINT);
		}
	}

	namespace
	{
		constexpr double cod4_weapon_landing_bob_scale = 0.1;

		std::string build_shortversion_string()
		{
			std::string version = VERSION_PRODUCT;

#ifdef DEBUG
			version += "-dbg";
#elif defined(NDEBUG)
			// release keeps the plain semantic version
#else
			version += "-nightly";
#endif

			return version;
		}

		std::string normalize_build_date(const char* raw_date)
		{
			if (!raw_date)
			{
				return {};
			}

			std::string date = raw_date;
			while (date.find("  ") != std::string::npos)
			{
				date.replace(date.find("  "), 2, " ");
			}

			return date;
		}

		std::string build_build_label()
		{
			std::string short_hash = GIT_HASH;
			if (short_hash.size() > 7)
			{
				short_hash.resize(7);
			}

			if (GIT_DIRTY)
			{
				short_hash += "-dirty";
			}

			return short_hash;
		}

		std::string build_timestamp_label()
		{
			return normalize_build_date(__DATE__) + " " + __TIME__;
		}

		std::string build_game_date_string()
		{
			return normalize_build_date(__DATE__);
		}

		std::string build_version_string()
		{
			return "Project: Consolation "
				+ build_shortversion_string()
				+ " build "
				+ build_build_label()
				+ " "
				+ build_timestamp_label()
				+ " win-x86";
		}

		int ret_one(DWORD*, int)
		{
			return 1;
		}

		void make_dvar_saved_and_writable(const char* name)
		{
			auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only | game::dvar_flags::write_protected);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | static_cast<std::uint16_t>(game::dvar_flags::saved));
		}

		bool is_windowed_borderless_requested()
		{
			const auto* const fullscreen = game::Dvar_FindVar("r_fullscreen");
			if (!fullscreen || fullscreen->current.enabled)
			{
				return false;
			}

			const auto* const borderless = dvars::r_borderless ? dvars::r_borderless : game::Dvar_FindVar("r_borderless");
			return borderless && borderless->current.enabled;
		}

		void apply_borderless_window_mode(const HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd) || !is_windowed_borderless_requested())
			{
				return;
			}

			RECT client_rect{};
			if (!GetClientRect(hwnd, &client_rect))
			{
				return;
			}

			const auto client_width = client_rect.right - client_rect.left;
			const auto client_height = client_rect.bottom - client_rect.top;

			auto style = GetWindowLongPtrA(hwnd, GWL_STYLE);
			auto ex_style = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);

			style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
			style |= WS_POPUP | WS_VISIBLE;
			ex_style &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);

			SetWindowLongPtrA(hwnd, GWL_STYLE, style);
			SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex_style);
			SetWindowPos(hwnd, nullptr, 0, 0, client_width, client_height,
				SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED);
		}

		void replace_float_register_call(const size_t callsite, const size_t target_global, const char* name, const char* description,
			const float value, const float min_value, const float max_value, const std::uint16_t flags = game::dvar_flags::saved)
		{
			utils::hook::nop(game::game_offset(callsite), 5);
			*reinterpret_cast<game::dvar_s**>(game::game_offset(target_global)) =
				dvars::Dvar_RegisterFloat(name, description, value, min_value, max_value, flags);
		}

		HWND __stdcall create_window_ex_stub(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
		{
			if (!strcmp(class_name, "JB_MP"))
			{
				window_name = "Project: Consolation - Multiplayer";
			}

			const auto hwnd = CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, inst, param);
			apply_borderless_window_mode(hwnd);
			return hwnd;
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
			if (type == game::DVAR_TYPE_FLOAT_2 && !_stricmp(dvarName, "cg_debugInfoCornerOffset"))
			{
				value.vector[0] = 0.0f;
				value.vector[1] = 0.0f;
			}

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

			if (type == game::DVAR_TYPE_STRING)
			{
				auto* var = find_dvar(dvars::overrides::register_string_overrides, dvarName);
				if (var)
				{
					value.string = var->value.c_str();
					flags = static_cast<unsigned short>(var->flags);
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
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// branding - intercept import for CreateWindowExA to change window title
			utils::hook::set(game::game_offset(0x1047627C), create_window_ex_stub);

			// nop call to Com_Printf for "SCALEFORM: %s" messages
			utils::hook::nop(game::game_offset(0x1000230F), 0x05); // TODO: Dvar toggle? Could be useful info
			utils::hook::nop(game::game_offset(0x102E1284), 0x05);
			// nop above call to Com_Printf for "unknown UI script %s in block:\n%s\n"

			// keep the registered version dvar value instead of letting stock init overwrite it
			utils::hook::nop(game::game_offset(0x103F9E53), 0x05);

			// stop an engine UI path from intentionally breaking into the debugger
			utils::hook::nop(game::game_offset(0x1027D3C4), 0x05);

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

			dvars::overrides::register_bool("sv_cheats", 1, game::dvar_flags::none);
			dvars::overrides::register_int("com_maxfps", 60, 0, 1000, game::dvar_flags::saved);
			dvars::overrides::register_int("g_speed", 210, 0, 1000, game::dvar_flags::saved); //cod4
			dvars::overrides::register_float("ui_smallFont", 0.0, 0, 1, game::dvar_flags::saved);
			dvars::overrides::register_float("ui_bigFont", 0.0, 0, 1, game::dvar_flags::saved);
			dvars::overrides::register_float("ui_extraBigFont", 0.0, 0, 1, game::dvar_flags::saved); 
			dvars::overrides::register_float("cg_overheadNamesSize", 0.5, 0, 1, game::dvar_flags::saved);
			dvars::overrides::register_float("input_viewSensitivity", 1.0f, 0.01f, 30.0f, game::dvar_flags::saved);
			dvars::overrides::register_string("version", build_version_string(),
				static_cast<unsigned int>(game::dvar_flags::server_info | game::dvar_flags::read_only));
			dvars::overrides::register_string("shortversion", build_shortversion_string(),
				static_cast<unsigned int>(game::dvar_flags::server_info | game::dvar_flags::read_only));
			dvars::overrides::register_string("gamename", "Project: Consolation",
				static_cast<unsigned int>(game::dvar_flags::read_only));
			dvars::overrides::register_string("gamedate", build_game_date_string(),
				static_cast<unsigned int>(game::dvar_flags::read_only));
			//dvars::overrides::register_float("r_lodScale", 0, 0, 3, game::dvar_flags::saved); //doesn't save
			//dvars::overrides::register_float("jump_height", 39.0, 0, 1000, game::dvar_flags::saved); //adjusted to 39 to allow cod4-like jump onto ledges

			
			dvar_registernew_hook.create(game::Dvar_RegisterNew, Dvar_RegisterNew_Stub);

			scheduler::once([]
			{
				utils::hook::nop(game::game_offset(0x103AF41F), 5);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054688)) = dvars::Dvar_RegisterFloat("r_lodScale", "Scale the level of detail distance (larger reduces detail)", 0, 0, 3, game::dvar_flags::saved);
				
				utils::hook::nop(game::game_offset(0x102BE942), 5);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148BECC)) = dvars::Dvar_RegisterFloat("cg_fovScale", "Scale applied to the field of view", 1, 0, 2, game::dvar_flags::saved); //resets on server start
				
				utils::hook::nop(game::game_offset(0x102BE908), 5);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148F6A4)) = dvars::Dvar_RegisterFloat("cg_fov", "The field of view angle in degrees", 65, 0, 160, game::dvar_flags::saved);
				
				utils::hook::nop(game::game_offset(0x101DB65A), 5);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x118EE1C0)) = dvars::Dvar_RegisterFloat("jump_height", "The maximum height of a player's jump", 41.f, 0, 1000.f, game::dvar_flags::saved);

				//dvars::Dvar_RegisterFloat("cg_fovScale", "Scale applied to the field of view", 1.0, 0, 2.0, game::dvar_flags::saved); //doesnt save

				utils::hook::nop(game::game_offset(0x103B2260), 5);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054944)) = dvars::Dvar_RegisterInt("developer", "Enable development environment", 0, 0, 2, game::dvar_flags::none);

				replace_float_register_call(0x102BF69A, 0x1148C590, "cg_gun_move_f", "Weapon movement forward due to player movement", 0.0f, 0.0f, 50.0f);
				replace_float_register_call(0x102BF6CF, 0x1148C574, "cg_gun_move_r", "Weapon movement right due to player movement", 0.0f, 0.0f, 50.0f);
				replace_float_register_call(0x102BF704, 0x1148F6D0, "cg_gun_move_u", "Weapon movement up due to player movement", 0.0f, 0.0f, 50.0f);
				replace_float_register_call(0x102BF877, 0x113F5F74, "cg_gun_move_rate", "The base weapon movement rate", 0.0f, 0.0f, 50.0f);
				replace_float_register_call(0x102BF8AC, 0x113F5EC0, "cg_gun_move_minspeed", "The minimum weapon movement rate", 0.0f, 0.0f, 1000.0f);
				replace_float_register_call(0x102BF8E1, 0x113F5E4C, "cg_gun_rot_rate", "The base weapon rotation rate", 0.0f, 0.0f, 50.0f);
				replace_float_register_call(0x102BF916, 0x1148C674, "cg_gun_rot_minspeed", "The minimum weapon rotation speed", 0.0f, 0.0f, 1000.0f);
				replace_float_register_call(0x102BF976, 0x1148C584, "cg_bobWeaponAmplitude", "The weapon bob amplitude", 0.16f, 0.0f, 10.0f);
				replace_float_register_call(0x102BF9A9, 0x113F5F80, "cg_bobWeaponRollAmplitude", "The amplitude of roll for weapon bobbing", 1.5f, 0.0f, 10.0f);
				replace_float_register_call(0x102BF9DC, 0x1148C5F8, "cg_bobWeaponMax", "The maximum weapon bob", 10.0f, 0.0f, 50.0f);
				replace_float_register_call(0x102BFA0F, 0x1148C638, "cg_bobWeaponLag", "The lag on the weapon bob", 0.25f, 0.0f, 10.0f);

				utils::hook::set<std::uint32_t>(game::game_offset(0x10297C44), static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&cod4_weapon_landing_bob_scale)));
				utils::hook::set<std::uint32_t>(game::game_offset(0x10297C79), static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&cod4_weapon_landing_bob_scale)));

				make_dvar_saved_and_writable("com_maxfps");
				make_dvar_saved_and_writable("sv_cheats");
				make_dvar_saved_and_writable("r_fullscreen");
				make_dvar_saved_and_writable("vid_xpos");
				make_dvar_saved_and_writable("vid_ypos");

				//debug block sv_cheats
#ifdef DEBUG
				utils::hook::nop(game::game_offset(0x101AB211), 5);
				utils::hook::nop(game::game_offset(0x10245A2A), 5);

				auto* const sv_cheats = dvars::Dvar_RegisterBool("sv_cheats", 1, "Enable Cheats", game::dvar_flags::none);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x11A343C0)) = sv_cheats;
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x1149FCD8)) = sv_cheats;
#endif


				//dvars::overrides::register_float("r_lodScale", 0, 0, 3, game::dvar_flags::saved); //doesn't save
			}, scheduler::main);

			scheduler::loop([]
			{
				make_dvar_saved_and_writable("com_maxfps");
				make_dvar_saved_and_writable("sv_cheats");
				make_dvar_saved_and_writable("r_fullscreen");
				make_dvar_saved_and_writable("vid_xpos");
				make_dvar_saved_and_writable("vid_ypos");
			}, scheduler::main, 250ms);
		}
	};
}

REGISTER_COMPONENT(patches::component)
