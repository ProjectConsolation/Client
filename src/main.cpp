#include <std_include.hpp>

#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include <utils/string.hpp>
#include <utils/flags.hpp>
#include <utils/io.hpp>
#include <utils/hook.hpp>

#include <ShellScalingApi.h>

#include "component/xlive.hpp"         // ADD - just this one include


namespace
{
	static BYTE original_code[5];
	static PBYTE original_ep = 0;

	DECLSPEC_NORETURN void WINAPI exit_hook(const int code)
	{
		component_loader::pre_destroy();
		exit(code);
	}

	void enable_dpi_awareness()
	{
		const utils::nt::library user32{"user32.dll"};

		{
			const auto set_dpi = user32
				? user32.get_proc<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
					"SetProcessDpiAwarenessContext")
				: nullptr;
			if (set_dpi)
			{
				set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
				return;
			}
		}

		{
			const utils::nt::library shcore{"shcore.dll"};
			const auto set_dpi = shcore
				? shcore.get_proc<HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS)>(
					"SetProcessDpiAwareness")
				: nullptr;
			if (set_dpi)
			{
				set_dpi(PROCESS_PER_MONITOR_DPI_AWARE);
				return;
			}
		}

		{
			const auto set_dpi = user32
				? user32.get_proc<BOOL(WINAPI*)()>(
					"SetProcessDPIAware")
				: nullptr;
			if (set_dpi)
			{
				set_dpi();
			}
		}
	}

	void unprotect_module(HMODULE module)
	{
		auto header = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
		auto nt_header = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<DWORD>(module) + header->e_lfanew);
		DWORD old_protect;
		VirtualProtect(reinterpret_cast<LPVOID>(module), nt_header->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READWRITE, &old_protect);
	}

	void main()
	{
		enable_dpi_awareness();
		srand(uint32_t(time(nullptr)));

		try
		{
#ifdef DEBUG
			xlive::apply_early(); // attemp at patching xlive FIRST, before a debugger can attach
			MessageBoxA(NULL, "ATTACH DEBUGGER NOW", "DEBUG", MB_DEFBUTTON1);
#endif

			
			// attach here - patches already in place, xlive already patched

			if (!component_loader::post_start()) throw "post start failed";
			if (!component_loader::post_load()) throw "post load failed";
#ifdef DEBUG
			MessageBoxA(NULL, "GAME LOADED", "DEBUG", MB_DEFBUTTON1);
#endif
		}
		catch (std::string& error)
		{
			component_loader::pre_destroy();
			MessageBoxA(nullptr, error.data(), "ERROR", MB_ICONERROR);
		}
	}
}

int WINAPI DllMain(HINSTANCE, const DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		main();
	}

	return 1;
}
