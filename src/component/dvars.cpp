#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <component/dvars.hpp>

namespace dvars
{
	game::qos::dvar_s* r_noborder = nullptr;

	//game::qos::dvar_s* M_RawInput = nullptr;

	// Gamepad
	//game::qos::dvar_s* gpad_use_hold_time = nullptr;

	namespace reg
	{

	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			//dvars::r_noborder = dvars::reg::Dvar_RegisterBool_r("r_noborder", "Do not use a border in windowed mode", false, game::qos::saved);
		}
	};
}