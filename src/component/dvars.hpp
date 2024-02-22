#pragma once
#include <std_include.hpp>
#include <game/symbols.hpp>

namespace dvars
{
	extern game::qos::dvar_s* r_noborder;

	//extern game::qos::dvar_s* M_RawInput;

	// Gamepad
	//extern game::qos::dvar_s* gpad_use_hold_time;

	namespace reg
	{
		inline game::qos::dvar_s* Dvar_RegisterBool_r(const char* dvar_name, const char* description, std::int32_t default_value, std::uint16_t flags)
		{
			//0 = bool
			return game::Dvar_RegisterBool(dvar_name, 0 , default_value);
		}
	}
}