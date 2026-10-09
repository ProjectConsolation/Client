#include "../src/component/engine/patches/ui_directive_abi.hpp"
#include <cassert>
#include <cstdio>

// Standalone x86 /RTC1 regression, not a build of the client. The fake native
// function models the verified register arguments and caller-clean RET.
static int directive_seen;
static char* output_seen;
static int context_seen;
static unsigned int flags_seen;
static char storage[8];

__declspec(naked) void native_directive()
{
	__asm
	{
		mov directive_seen, ecx
		mov output_seen, edx
		mov eax, [esp + 4]
		mov context_seen, eax
		mov eax, [esp + 8]
		mov flags_seen, eax
		mov eax, edx
		ret
	}
}

char* __cdecl guard(int directive, char* output, int context, unsigned char flags)
{
	// Model the guard's early-rejection return without entering native code.
	if (directive == -1) return output;
	return patches::ui_directive_abi::call_native(
		reinterpret_cast<std::uintptr_t>(native_directive), directive, output, context, flags);
}

__declspec(naked) void entry()
{
	__asm
	{
		push dword ptr[esp + 8]
		push dword ptr[esp + 8]
		push edx
		push ecx
		call guard
		add esp, 16
		ret
	}
}

int main()
{
	for (int i = 0; i < 10000; ++i)
	{
		unsigned int before, after;
		__asm mov before, esp
		const auto result = patches::ui_directive_abi::call_native(
			reinterpret_cast<std::uintptr_t>(entry), 0x12345678, storage, -17, 0xFE);
		__asm mov after, esp
		assert(before == after);
		assert(result == storage);
		assert(directive_seen == 0x12345678);
		assert(output_seen == storage);
		assert(context_seen == -17);
		assert(flags_seen == 0xFE);
		__asm mov before, esp
		const auto rejected = patches::ui_directive_abi::call_native(
			reinterpret_cast<std::uintptr_t>(entry), -1, storage, 42, 1);
		__asm mov after, esp
		assert(before == after);
		assert(rejected == storage);
		assert(directive_seen == 0x12345678);
	}
	std::puts("UI directive register/caller-clean ABI tests passed");
}
