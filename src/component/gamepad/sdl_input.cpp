#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "gamepad.hpp"

namespace sdl_input
{
	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// SDL backend scaffold; left inert until SDL is added as a dependency.
		}
	};
}

REGISTER_COMPONENT(sdl_input::component)
