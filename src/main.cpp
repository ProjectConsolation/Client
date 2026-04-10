#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include <utils/string.hpp>
#include <utils/flags.hpp>
#include <utils/io.hpp>
#include <utils/nt.hpp>
#include <utils/hook.hpp>
#include <ShellScalingApi.h>
#include <optional>
#include "component/engine/patches/xlive.hpp"

#ifdef DEBUG
#include <crtdbg.h>
#endif

namespace
{
	static BYTE original_code[5];
	static PBYTE original_ep = 0;
	constexpr auto unsupported_update_url = "https://placeholder.link";
	constexpr std::uint32_t multiplayer_marker_rva = 0x0050502C;

	bool has_supported_multiplayer_marker(const std::filesystem::path& game_root)
	{
		const auto module_path = game_root / "jb_mp_s.dll";
		const auto marker = utils::io::read_pe_string_rva(module_path.generic_string(), multiplayer_marker_rva);
		return marker.has_value() && _stricmp(marker->c_str(), "multiplayer") == 0;
	}

	std::string read_multiplayer_marker_for_log(const std::filesystem::path& game_root)
	{
		const auto module_path = game_root / "jb_mp_s.dll";
		const auto marker = utils::io::read_pe_string_rva(module_path.generic_string(), multiplayer_marker_rva);
		return marker.value_or("<missing>");
	}

	DECLSPEC_NORETURN void show_unsupported_version_and_exit()
	{
		const auto result = MessageBoxA(nullptr,
			"Project: Consolation does not support Quantum of Solace 1.0.\n\n"
			"Update to 1.1 via:\n"
			"https://placeholder.link\n\n"
			"Press Yes to open the update page.\n"
			"Consolation will now exit.",
			"Project: Consolation",
			MB_ICONERROR | MB_YESNO | MB_DEFBUTTON1);

		if (result == IDYES)
		{
			ShellExecuteA(nullptr, "open", unsupported_update_url, nullptr, nullptr, SW_SHOWNORMAL);
		}

		utils::nt::terminate(1);
	}

	void validate_supported_install()
	{
		const auto game_root = std::filesystem::path(utils::nt::get_host_module().get_folder());
		const auto marker = read_multiplayer_marker_for_log(game_root);
		printf("consolation: jb_mp_s.dll marker @ 0x%08X = '%s'\n", multiplayer_marker_rva, marker.c_str());

		if (!has_supported_multiplayer_marker(game_root))
		{
			printf("consolation: unsupported install detected, aborting startup\n");
			show_unsupported_version_and_exit();
		}

		printf("consolation: supported multiplayer marker detected, continuing startup\n");
	}

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

#ifdef DEBUG
	void configure_debug_crt()
	{
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
	}
#endif

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
		validate_supported_install();

		try
		{
#ifdef DEBUG
			configure_debug_crt();
			//xlive::apply_early(); // attemp at patching xlive FIRST, before a debugger can attach
			//MessageBoxA(NULL, "ATTACH DEBUGGER NOW", "DEBUG", MB_DEFBUTTON1);
#endif

			
			// attach here - patches already in place, xlive already patched

			if (!component_loader::post_start()) throw "post start failed";
			if (!component_loader::post_load()) throw "post load failed";
#ifdef DEBUG
			//MessageBoxA(NULL, "GAME LOADED", "DEBUG", MB_DEFBUTTON1);
#endif
		}
		catch (const std::exception& error)
		{
			component_loader::pre_destroy();
			MessageBoxA(nullptr, error.what(), "ERROR", MB_ICONERROR);
		}
		catch (const char* error)
		{
			component_loader::pre_destroy();
			MessageBoxA(nullptr, error, "ERROR", MB_ICONERROR);
		}
		catch (const std::string& error)
		{
			component_loader::pre_destroy();
			MessageBoxA(nullptr, error.data(), "ERROR", MB_ICONERROR);
		}
		catch (...)
		{
			component_loader::pre_destroy();
			MessageBoxA(nullptr, "Unknown startup failure", "ERROR", MB_ICONERROR);
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
