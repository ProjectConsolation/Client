#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <utils/hook.hpp>
#include <utils/nt.hpp>

#include "resources.hpp"

namespace resources
{
	namespace
	{
		HICON icon{};
		HBITMAP splash{};
		HBITMAP console_logo{};
		utils::hook::detour load_image_a_hook;
		utils::hook::detour load_icon_a_hook;
		using load_image_a_fn = HANDLE(WINAPI*)(HINSTANCE, LPCSTR, UINT, int, int, UINT);
		using load_icon_a_fn = HICON(WINAPI*)(HINSTANCE, LPCSTR);

		bool is_named_resource(const LPCSTR name, const char* expected)
		{
			if (!name || IS_INTRESOURCE(name))
			{
				return false;
			}

			const auto backslash = strrchr(name, '\\');
			const auto forward_slash = strrchr(name, '/');
			const auto slash = !backslash || (forward_slash && forward_slash > backslash)
				? forward_slash
				: backslash;
			return _stricmp(slash ? slash + 1 : name, expected) == 0;
		}

		bool is_integer_resource(const LPCSTR name, const WORD expected)
		{
			return name && IS_INTRESOURCE(name) && reinterpret_cast<ULONG_PTR>(name) == expected;
		}

		bool is_game_module(const HINSTANCE handle)
		{
			return handle == GetModuleHandleW(nullptr) || handle == GetModuleHandleA("jb_mp_s.dll");
		}

		bool is_game_icon_request(const HINSTANCE handle, const LPCSTR name)
		{
			if (!name || !IS_INTRESOURCE(name))
			{
				return false;
			}

			const auto resource_id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
			return is_game_module(handle) &&
				(resource_id == 1 || resource_id == 2 || resource_id == 101 || resource_id == 102);
		}

		HANDLE copy_image_or_original(const HANDLE image, const UINT type, const int width, const int height)
		{
			const auto copy = CopyImage(image, type, width, height, 0);
			return copy ? copy : image;
		}

		HANDLE WINAPI load_image_a(const HINSTANCE handle, LPCSTR name, const UINT type, const int c_x, const int c_y,
			const UINT load)
		{
			if (type == IMAGE_ICON && is_game_icon_request(handle, name) && icon)
			{
				return copy_image_or_original(icon, IMAGE_ICON, c_x, c_y);
			}

			if (type == IMAGE_BITMAP && splash &&
				((is_game_module(handle) && is_integer_resource(name, 0x64)) || is_named_resource(name, "jb.bmp")))
			{
				// The original launcher requests its stock bitmap dimensions. Resizing
				// the embedded 768x480 replacement to that request makes it appear
				// cropped/zoomed inside the native-sized splash window.
				return copy_image_or_original(splash, IMAGE_BITMAP, 0, 0);
			}

			if (type == IMAGE_BITMAP && console_logo &&
				(is_named_resource(name, "logo.bmp") || is_named_resource(name, "console.bmp") ||
					is_named_resource(name, "jblogo.bmp")))
			{
				return copy_image_or_original(console_logo, IMAGE_BITMAP, c_x, c_y);
			}

			const auto original = reinterpret_cast<load_image_a_fn>(load_image_a_hook.get_original());
			return original(handle, name, type, c_x, c_y, load);
		}

		HICON WINAPI load_icon_a(const HINSTANCE handle, const LPCSTR name)
		{
			if (is_game_icon_request(handle, name) && icon)
			{
				const auto copy = CopyIcon(icon);
				return copy ? copy : icon;
			}

			const auto original = reinterpret_cast<load_icon_a_fn>(load_icon_a_hook.get_original());
			return original(handle, name);
		}
	}

	void apply_window_icon(const HWND window)
	{
		if (!window || !icon)
		{
			return;
		}

		SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
		SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
	}

	void prepare_splash_thread()
	{
		static thread_local bool configured = false;
		if (configured)
		{
			return;
		}

		const utils::nt::library user32{"user32.dll"};
		const auto set_thread_dpi = user32
			? user32.get_proc<DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
				"SetThreadDpiAwarenessContext")
			: nullptr;
		if (set_thread_dpi && set_thread_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
		{
			configured = true;
		}
	}

	bool get_splash_dimensions(int& width, int& height)
	{
		BITMAP bitmap{};
		if (!splash || GetObjectA(splash, sizeof(bitmap), &bitmap) != sizeof(bitmap))
		{
			return false;
		}

		width = bitmap.bmWidth;
		height = std::abs(bitmap.bmHeight);
		return width > 0 && height > 0;
	}

	class component final : public component_interface
	{
	public:
		~component() override
		{
			if (icon) DestroyIcon(icon);
			if (console_logo) DeleteObject(console_logo);
			if (splash) DeleteObject(splash);
		}

		void post_start() override
		{
			const auto self = utils::nt::library::get_by_address(reinterpret_cast<void*>(load_image_a));

			icon = static_cast<HICON>(LoadImageA(self.get_handle(), MAKEINTRESOURCEA(ID_ICON), IMAGE_ICON,
				0, 0, LR_DEFAULTSIZE));
			splash = static_cast<HBITMAP>(LoadImageA(self.get_handle(), MAKEINTRESOURCEA(IMAGE_SPLASH),
				IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
			console_logo = static_cast<HBITMAP>(LoadImageA(self.get_handle(), MAKEINTRESOURCEA(IMAGE_CONSOLE_LOGO),
				IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));

			if (!icon || !splash || !console_logo)
			{
				throw std::runtime_error(std::format(
					"failed to load embedded resources (icon={}, splash={}, console={})",
					icon != nullptr, splash != nullptr, console_logo != nullptr));
			}
		}

		void post_load() override
		{
			load_image_a_hook.create(reinterpret_cast<void*>(LoadImageA), load_image_a);
			load_icon_a_hook.create(reinterpret_cast<void*>(LoadIconA), load_icon_a);
			std::printf("[resources] embedded icon, splash, and console image overrides installed\n");
		}

		void pre_destroy() override
		{
			load_icon_a_hook.clear();
			load_image_a_hook.clear();
		}
	};
}

REGISTER_COMPONENT(resources::component)
