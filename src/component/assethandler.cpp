#include <std_include.hpp>
#include "loader/component_loader.hpp"

namespace assethandler
{
	namespace
	{

	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{

		}
	};
}

REGISTER_COMPONENT(assethandler::component)
