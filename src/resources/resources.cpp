#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <utils/nt.hpp>

namespace resources
{
	namespace
	{
		HANDLE splash, logo;
	}

	class component final : public component_interface
	{
	public:
		~component() override
		{
			if (splash) DeleteObject(splash);
		}

		void post_start() override
		{
			const utils::nt::library self;

			splash = LoadImageA(self.get_handle(), MAKEINTRESOURCEA(IMAGE_SPLASH), IMAGE_BITMAP, 0, 0, LR_COPYFROMRESOURCE);
		}

		void* load_import(const std::string& library, const std::string& function) override
		{
			return nullptr;
		}
	};
}

REGISTER_COMPONENT(resources::component)
