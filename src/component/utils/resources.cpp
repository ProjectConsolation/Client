#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <utils/hook.hpp>
#include <utils/nt.hpp>

#include "resources.hpp"
#include <unordered_set>
#include <mutex>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")

namespace resources
{
	namespace
	{
		HICON icon{};
		HBITMAP splash{};
		HBITMAP console_logo{};
		constexpr char splash_class[] = "ConsolationSplashBitmap";
		bool splash_class_registered{};
		std::unordered_set<HBITMAP> splash_copies;
		std::mutex splash_copies_mutex;

		void apply_taskbar_identity(HWND window)
		{
			// WM_SETICON does not identify a pinned shortcut. Use the same explicit
			// Shell identity as the supplied shortcut helper; no executable patching.
			IPropertyStore* store{};
			if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store)))) return;
			const auto set = [store](const PROPERTYKEY& key, const wchar_t* text)
			{
				PROPVARIANT value{};
				if (SUCCEEDED(InitPropVariantFromString(text, &value)))
				{
					store->SetValue(key, value);
					PropVariantClear(&value);
				}
			};
			constexpr PROPERTYKEY id = INIT_PKEY_AppUserModel_ID;
			constexpr PROPERTYKEY icon_key = INIT_PKEY_AppUserModel_RelaunchIconResource;
			constexpr PROPERTYKEY name_key = INIT_PKEY_AppUserModel_RelaunchDisplayNameResource;
			constexpr PROPERTYKEY command_key = INIT_PKEY_AppUserModel_RelaunchCommand;
			wchar_t module_path[MAX_PATH]{};
			const auto self = utils::nt::library::get_by_address(reinterpret_cast<void*>(apply_taskbar_identity));
			if (GetModuleFileNameW(self.get_handle(), module_path, MAX_PATH))
			{
				const auto icon_resource = std::wstring(module_path) + L",-102";
				set(icon_key, icon_resource.c_str());
			}
			set(name_key, L"Project: Consolation");
			// Retain this instance's actual offline/profile arguments on relaunch.
			set(command_key, GetCommandLineW());
			set(id, L"ProjectConsolation.Client.Multiplayer");
			store->Release();
		}

		LRESULT CALLBACK splash_bitmap_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
		{
			// QoS 102C3280 sends STM_SETIMAGE then reads the child rectangle.
			// Own only this startup bitmap child: no SS_BITMAP automatic sizing
			// or DPI-dependent image scaling, and always draw the entire image.
			if (message == STM_SETIMAGE && wparam == IMAGE_BITMAP)
			{
				const auto old = GetWindowLongPtrW(window, GWLP_USERDATA);
				SetWindowLongPtrW(window, GWLP_USERDATA, lparam);
				InvalidateRect(window, nullptr, FALSE);
				return old;
			}
			if (message == WM_ERASEBKGND) return 1;
			if (message == WM_SIZE) InvalidateRect(window, nullptr, FALSE);
			if (message == WM_PAINT)
			{
				PAINTSTRUCT paint{};
				const auto dc = BeginPaint(window, &paint);
				RECT rect{};
				GetClientRect(window, &rect);
				FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
				const auto bitmap = reinterpret_cast<HBITMAP>(GetWindowLongPtrW(window, GWLP_USERDATA));
				BITMAP dimensions{};
				if (bitmap && GetObject(bitmap, sizeof(dimensions), &dimensions) && dimensions.bmWidth > 0 && dimensions.bmHeight != 0)
				{
					const auto source = CreateCompatibleDC(dc);
					if (source)
					{
						const auto old = SelectObject(source, bitmap);
						const float scale = std::min(static_cast<float>(rect.right) / dimensions.bmWidth,
							static_cast<float>(rect.bottom) / std::abs(dimensions.bmHeight));
						const int width = static_cast<int>(dimensions.bmWidth * scale);
						const int height = static_cast<int>(std::abs(dimensions.bmHeight) * scale);
						SetStretchBltMode(dc, HALFTONE);
						SetBrushOrgEx(dc, 0, 0, nullptr);
						StretchBlt(dc, (rect.right - width) / 2, (rect.bottom - height) / 2, width, height,
							source, 0, 0, dimensions.bmWidth, std::abs(dimensions.bmHeight), SRCCOPY);
						SelectObject(source, old);
						DeleteDC(source);
					}
				}
				EndPaint(window, &paint);
				return 0;
			}
			return DefWindowProcA(window, message, wparam, lparam);
		}
		utils::hook::detour load_image_a_hook;
		utils::hook::detour load_icon_a_hook;
		utils::hook::detour send_message_a_hook;
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
				const auto copy = static_cast<HBITMAP>(copy_image_or_original(splash, IMAGE_BITMAP, 0, 0));
				std::lock_guard lock(splash_copies_mutex);
				splash_copies.insert(copy);
				return copy;
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

		LRESULT WINAPI send_message_a(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
		{
			bool replacement = false;
			if (message == STM_SETIMAGE && wparam == IMAGE_BITMAP)
			{
				std::lock_guard lock(splash_copies_mutex);
				replacement = splash_copies.contains(reinterpret_cast<HBITMAP>(lparam));
			}
			if (replacement)
			{
				char class_name[64]{};
				GetClassNameA(window, class_name, sizeof(class_name));
				if (!_stricmp(class_name, "Static"))
				{
					// Catch bitmap controls outside the patched QoS IAT too. SS_BITMAP
					// may DPI-scale its bitmap independently of its fixed client size.
					// Only handles returned by our splash override qualify for this path.
					SetLastError(0);
					const auto previous = SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(splash_bitmap_proc));
					if (previous || GetLastError() == 0)
					{
						SetWindowLongPtrA(window, GWL_STYLE, GetWindowLongPtrA(window, GWL_STYLE) & ~0xFFFFL);
						std::printf("[resources] splash bitmap control uses aspect-fit painting\n");
						return splash_bitmap_proc(window, message, wparam, lparam);
					}
				}
			}
			using send_message_fn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
			const auto original = reinterpret_cast<send_message_fn>(send_message_a_hook.get_original());
			return original(window, message, wparam, lparam);
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
		apply_taskbar_identity(window);
	}

	const char* splash_control_class()
	{
		return splash_class_registered ? splash_class : "Static";
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
			WNDCLASSA bitmap_class{};
			bitmap_class.style = CS_GLOBALCLASS; // Created by the game's HINSTANCE.
			bitmap_class.lpfnWndProc = splash_bitmap_proc;
			bitmap_class.hInstance = self.get_handle();
			bitmap_class.lpszClassName = splash_class;
			splash_class_registered = RegisterClassA(&bitmap_class) != 0;

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
			send_message_a_hook.create(reinterpret_cast<void*>(SendMessageA), send_message_a);
			std::printf("[resources] embedded icon, splash, and console image overrides installed\n");
		}

		void pre_destroy() override
		{
			load_icon_a_hook.clear();
			send_message_a_hook.clear();
			load_image_a_hook.clear();
		}
	};
}

REGISTER_COMPONENT(resources::component)
