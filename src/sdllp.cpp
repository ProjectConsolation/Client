#include <std_include.hpp>

#include "sdllp.hpp"

#include <utils/flags.hpp>
#include <utils/string.hpp>
#include <utils/nt.hpp>

#define EXPORT(_export) extern "C" __declspec(naked) __declspec(dllexport) void _export() { static FARPROC function = 0; if (!function) function = sdllp::get_export(__FUNCTION__, LIBRARY); __asm { jmp function } }  

std::map<std::string, HINSTANCE> sdllp::libraries;

namespace
{
	bool singleplayer_warning_shown = false;

	bool is_multiplayer_host()
	{
		const auto host_name = utils::nt::get_host_module().get_name();
		return !_stricmp(host_name.c_str(), "JB_Launcher_s.exe")
			|| utils::flags::has_flag("multiplayer");
	}

	bool should_load_proxy_library(const char* library)
	{
		if (!_stricmp(library, "d3d9.dll"))
		{
			return is_multiplayer_host();
		}

		return true;
	}

	void show_singleplayer_passthrough_warning(const char* library)
	{
		if (singleplayer_warning_shown || _stricmp(library, "d3d9.dll") != 0)
		{
			return;
		}

		singleplayer_warning_shown = true;
		MessageBoxA(
			nullptr,
			"Project: Consolation does not support singleplayer.\n\nLaunching without modifications using the default system d3d9.dll.",
			"Project: Consolation",
			MB_OK | MB_ICONWARNING
		);
	}
}

void sdllp::load_library(const char* library)
{
	char mPath[MAX_PATH];

	GetSystemDirectoryA(mPath, MAX_PATH);
	strcat_s(mPath, "\\");
	strcat_s(mPath, library);

	libraries[library] = LoadLibraryA(mPath);

	if (!is_loaded(library))
	{
		MessageBoxA(nullptr, utils::string::va("failed to load '%s'", library), "export", MB_ICONERROR);
	}
	else if (!should_load_proxy_library(library))
	{
		show_singleplayer_passthrough_warning(library);
		printf("export proxy for %s is running in passthrough mode (singleplayer)\n", library);
	}
}

bool sdllp::is_loaded(const char* library)
{
	return (libraries.find(library) != libraries.end() && libraries[library]);
}

FARPROC sdllp::get_export(const char* function, const char* library)
{
	printf("export '%s' requested from %s.\n", function, library);

	if (!is_loaded(library))
	{
		load_library(library);
	}

	auto address = GetProcAddress(libraries[library], function);
	if (!address)
	{
		MessageBoxA(nullptr, utils::string::va("unable to export function '%s' from '%s'", function, library), "export", MB_ICONERROR);
		return nullptr;
	}

	return address;
}

#define LIBRARY "d3d9.dll"

// C4740: inline asm in naked functions suppresses global optimization — expected behavior here
#pragma warning(push)
#pragma warning(disable: 4740)
EXPORT(D3DPERF_BeginEvent)
EXPORT(D3DPERF_EndEvent)
EXPORT(Direct3DCreate9)
#pragma warning(pop)
