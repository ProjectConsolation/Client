#pragma once
#include <cstddef>

namespace killcam_hud::abi
{
	// QoS PC 1.1 CG_Draw2dHudElems (102BC580): localClientNum in ECX,
	// foreground on the stack, caller cleanup. This is NOT cdecl(int), nor
	// MSVC fastcall (which would clean the stack argument). Retire with a
	// replacement renderer. KisakCOD's two-argument API is comparative only.
	inline std::size_t draw_foreground(void* target, int local_client, int foreground)
	{
		std::size_t result;
		__asm
		{
			mov ecx, local_client
			push foreground
			call target
			add esp, 4
			mov result, eax
		}
		return result;
	}
}
