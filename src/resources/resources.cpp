#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <utils/nt.hpp>

namespace resources
{
	namespace
	{
		HANDLE splash;
	}

	HANDLE WINAPI load_image_a(const HINSTANCE handle, LPCSTR name, const UINT type, const int cx, const int cy,
		const UINT load)
	{
		const utils::nt::library self;
		if (self.get_handle() == handle && name == LPCSTR(0x64)) return splash;

		return LoadImageA(handle, name, type, cx, cy, load);
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
			splash = LoadImageA(self.get_handle(), MAKEINTRESOURCEA(IMAGE_SPLASH), 0, 0, 0, LR_COPYFROMRESOURCE);
		}

		void* load_import(const std::string& library, const std::string& function) override
		{
			if (library == "USER32.dll")
			{
				if (function == "LoadImageA")
				{
					return load_image_a;
				}
			}

			return nullptr;
		}
	};
}

REGISTER_COMPONENT(resources::component)
