#include <std_include.hpp>

#include "sdllp.hpp"

#include <utils/string.hpp>
#include <utils/nt.hpp>

#define EXPORT(_export) extern "C" __declspec(naked) __declspec(dllexport) void _export() { static FARPROC function = 0; if (!function) function = sdllp::get_export(__FUNCTION__, LIBRARY); __asm { jmp function } }  

std::map<std::string, HINSTANCE> sdllp::libraries;

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
EXPORT(D3DPERF_BeginEvent)
EXPORT(D3DPERF_EndEvent)
EXPORT(Direct3DCreate9)
