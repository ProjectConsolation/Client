#include "std_include.hpp"

#ifndef DEBUG
#pragma comment(linker, "/merge:.text=.UPX0")
#pragma comment(linker, "/merge:.data=.UPX1")
#pragma comment(linker, "/merge:.rdata=.UPX2")
#pragma comment(linker, "/merge:.tls=.UPX3")
#pragma comment(linker, "/merge:.gfids=.UPX4")
#endif

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern "C"
{
	// Disable telemetry data logging
	void __cdecl __vcrt_initialize_telemetry_provider() {}
	void __cdecl __telemetry_main_invoke_trigger() {}
	void __cdecl __telemetry_main_return_trigger() {}
	void __cdecl __vcrt_uninitialize_telemetry_provider() {}

	// Enable 'High Performance Graphics'
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;

	// Tommath fixes
	int s_read_arc4random(void*, size_t)
	{
		return -1;
	}

	int s_read_getrandom(void*, size_t)
	{
		return -1;
	}

	int s_read_urandom(void*, size_t)
	{
		return -1;
	}

	int s_read_ltm_rng(void*, size_t)
	{
		return -1;
	}
}
