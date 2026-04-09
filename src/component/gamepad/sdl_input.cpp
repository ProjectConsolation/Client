#include <std_include.hpp>

#include "loader/component_loader.hpp"

namespace sdl_input
{
	class component final : public component_interface
	{
	public:
		void post_load() override
		{
		}
	};
}

REGISTER_COMPONENT(sdl_input::component)
