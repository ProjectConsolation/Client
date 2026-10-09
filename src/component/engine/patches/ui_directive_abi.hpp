#pragma once
#include <cstdint>

namespace patches::ui_directive_abi
{
	// QoS PC 1.1 102BB870: ECX = directive, EDX = output; context and
	// flags are stack arguments. RET at 102BBC2E does NOT pop them.
	// Callers 102BBCF8 / 1040DAF4 clean them alongside preceding CRT args.
	// This is a register/caller-clean ABI, not MSVC __fastcall.
	inline char* call_native(const std::uintptr_t target, const int directive,
		char* output, const int context, const unsigned char flags)
	{
		char* result;
		__asm
		{
			movzx eax, flags
			push eax
			push context
			mov edx, output
			mov ecx, directive
			call dword ptr[target]
			add esp, 8
			mov result, eax
		}
		return result;
	}
}
