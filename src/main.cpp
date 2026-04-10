#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include <utils/string.hpp>
#include <utils/flags.hpp>
#include <utils/io.hpp>
#include <utils/nt.hpp>
#include <utils/hook.hpp>
#include <ShellScalingApi.h>
#include <bcrypt.h>
#include <optional>
#include "component/engine/patches/xlive.hpp"

#ifdef DEBUG
#include <crtdbg.h>
#endif

#pragma comment(lib, "bcrypt.lib")

namespace
{
	static BYTE original_code[5];
	static PBYTE original_ep = 0;
	constexpr auto unsupported_update_url = "https://placeholder.link";
	constexpr auto unsupported_jb_mp_1_0_sha256 = "0C99BF76C63C4EB6D5672618514873EBB34D1603254B33676D6F31D8FD6195A1";

	bool should_enable_consolation_proxy()
	{
		return utils::flags::has_flag("multiplayer");
	}

	std::string bytes_to_hex_upper(const unsigned char* bytes, const size_t size)
	{
		static constexpr auto hex = "0123456789ABCDEF";

		std::string result;
		result.reserve(size * 2);

		for (size_t i = 0; i < size; ++i)
		{
			const auto value = bytes[i];
			result.push_back(hex[(value >> 4) & 0xF]);
			result.push_back(hex[value & 0xF]);
		}

		return result;
	}

	std::optional<std::string> sha256_file(const std::filesystem::path& path)
	{
		std::string data;
		if (!utils::io::read_file(path.generic_string(), &data))
		{
			return std::nullopt;
		}

		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		DWORD object_size = 0;
		DWORD hash_size = 0;
		DWORD bytes_copied = 0;

		auto close_handles = [&]()
		{
			if (hash)
			{
				BCryptDestroyHash(hash);
				hash = nullptr;
			}

			if (algorithm)
			{
				BCryptCloseAlgorithmProvider(algorithm, 0);
				algorithm = nullptr;
			}
		};

		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
		{
			close_handles();
			return std::nullopt;
		}

		if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
			sizeof(object_size), &bytes_copied, 0) != 0)
		{
			close_handles();
			return std::nullopt;
		}

		if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
			sizeof(hash_size), &bytes_copied, 0) != 0)
		{
			close_handles();
			return std::nullopt;
		}

		std::vector<unsigned char> object_buffer(object_size);
		std::vector<unsigned char> hash_buffer(hash_size);

		if (BCryptCreateHash(algorithm, &hash, object_buffer.data(),
			static_cast<ULONG>(object_buffer.size()), nullptr, 0, 0) != 0)
		{
			close_handles();
			return std::nullopt;
		}

		if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(data.data()),
			static_cast<ULONG>(data.size()), 0) != 0)
		{
			close_handles();
			return std::nullopt;
		}

		if (BCryptFinishHash(hash, hash_buffer.data(), static_cast<ULONG>(hash_buffer.size()), 0) != 0)
		{
			close_handles();
			return std::nullopt;
		}

		close_handles();
		return bytes_to_hex_upper(hash_buffer.data(), hash_buffer.size());
	}

	bool is_unsupported_1_0_install(const std::filesystem::path& game_root)
	{
		const auto module_path = game_root / "jb_mp_s.dll";
		const auto hash = sha256_file(module_path);
		return hash.has_value() && *hash == unsupported_jb_mp_1_0_sha256;
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
		if (is_unsupported_1_0_install(game_root))
		{
			show_unsupported_version_and_exit();
		}
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
		if (should_enable_consolation_proxy())
		{
			main();
		}
	}

	return 1;
}
