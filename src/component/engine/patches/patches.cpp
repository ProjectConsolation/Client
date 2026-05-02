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
			return std::string(__DATE__) + " " + __TIME__;
		}

		std::string build_game_date_string()
		{
			return __DATE__;
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

		bool dvar_enabled(const char* name)
		{
			const auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return false;
			}

			switch (dvar->type)
			{
			case game::dvar_type::boolean:
				return dvar->current.enabled;
			case game::dvar_type::integer:
				return dvar->current.integer != 0;
			case game::dvar_type::value:
				return dvar->current.value != 0.0f;
			case game::dvar_type::string:
			case game::dvar_type::enumeration:
				if (!dvar->current.string)
				{
					return false;
				}

				return dvar->current.string[0] != '\0'
					&& strcmp(dvar->current.string, "0") != 0
					&& _stricmp(dvar->current.string, "false") != 0
					&& _stricmp(dvar->current.string, "off") != 0;
			default:
				return dvar->current.integer != 0;
			}
		}

		int dvar_int_value(const char* name, const int fallback = 0)
		{
			const auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return fallback;
			}

			switch (dvar->type)
			{
			case game::dvar_type::boolean:
				return dvar->current.enabled ? 1 : 0;
			case game::dvar_type::integer:
				return dvar->current.integer;
			case game::dvar_type::value:
				return static_cast<int>(dvar->current.value);
			case game::dvar_type::string:
			case game::dvar_type::enumeration:
				return dvar->current.string ? std::atoi(dvar->current.string) : fallback;
			default:
				return dvar->current.integer;
			}
		}

		void make_dvar_saved_and_writable(const char* name)
		{
			auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | static_cast<std::uint16_t>(game::dvar_flags::saved));
		}

		HWND __stdcall create_window_ex_stub(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
		{
			if (!strcmp(class_name, "JB_MP"))
			{
				window_name = "Project: Consolation - Multiplayer";

				const auto fullscreen = dvar_enabled("r_fullscreen");
				const bool borderless = dvar_enabled("r_borderless");

				if (!fullscreen)
				{
					x = dvar_int_value("vid_xpos", x);
					y = dvar_int_value("vid_ypos", y);
				}

				if (!fullscreen && borderless)
				{
					style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
					style |= WS_POPUP;
					ex_style &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
					ex_style |= WS_EX_APPWINDOW;
				}
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

			if (type == game::DVAR_TYPE_ENUM)
			{
				if (!_stricmp(dvarName, "cg_drawFPS"))
				{
					const auto writable_flags = flags
						& ~static_cast<unsigned short>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);
					flags = static_cast<unsigned short>(writable_flags | static_cast<unsigned short>(game::dvar_flags::saved));
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

			// stop the video restart path from forcibly setting r_fullscreen back to 1
			utils::hook::nop(game::game_offset(0x103BE16D), 0x05);

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
				dvars::replace_dvar_at(game::game_offset(0x103AF41F), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054688)),
					dvars::make_float("r_lodScale", "Scale the level of detail distance (larger reduces detail)", 0.0f, 0.0f, 3.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x102BE942), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148BECC)),
					dvars::make_float("cg_fovScale", "Scale applied to the field of view", 1.0f, 0.0f, 2.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x102BE908), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148F6A4)),
					dvars::make_float("cg_fov", "The field of view angle in degrees", 65.0f, 0.0f, 160.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x101DB65A), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x118EE1C0)),
					dvars::make_float("jump_height", "The maximum height of a player's jump", 41.0f, 0.0f, 1000.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x10321221), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11260BD0)),
					dvars::make_float("input_viewSensitivity", "Mouse sensitivity", 1.0f, 0.01f, 30.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x103B2260), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054944)),
					dvars::make_int("developer", "Enable development environment", 0, 0, 2, game::dvar_flags::none));

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
