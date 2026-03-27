#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

namespace xlive
{
    namespace
    {
        utils::hook::detour ntqip_hook;

        // Always returns FALSE — xlive checks this before network init
        static BOOL WINAPI debugger_stub() { return FALSE; }

        LONG __stdcall ntqip_stub(HANDLE hProcess, UINT infoClass,
            PVOID pInfo, ULONG infoLen, PULONG pRetLen)
        {
            if (infoClass == 7)
            {
                if (pInfo && infoLen >= sizeof(ULONG_PTR))
                    *static_cast<ULONG_PTR*>(pInfo) = 0;
                if (pRetLen) *pRetLen = sizeof(ULONG_PTR);
                return 0;
            }
            if (infoClass == 0x1E)
            {
                if (pInfo && infoLen >= sizeof(HANDLE))
                    *static_cast<HANDLE*>(pInfo) = nullptr;
                return static_cast<LONG>(0xC0000353);
            }
            return static_cast<LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG, PULONG)>(
                ntqip_hook.get_original())(hProcess, infoClass, pInfo, infoLen, pRetLen);
        }

        bool mem_write(void* dst, const void* src, size_t len)
        {
            DWORD old = 0;
            if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old))
                return false;
            memcpy(dst, src, len);
            VirtualProtect(dst, len, old, &old);
            return true;
        }

        uint8_t* scan(uint8_t* base, size_t size, const uint8_t* pat, size_t pat_len)
        {
            if (!pat_len || size < pat_len) return nullptr;
            for (size_t i = 0, lim = size - pat_len; i <= lim; ++i)
                if (!memcmp(base + i, pat, pat_len))
                    return base + i;
            return nullptr;
        }

        // Walk xlive's PE import table to find and redirect IsDebuggerPresent.
        void patch_isdebugger(uint8_t* base, size_t image_size)
        {
            __try
            {
                const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

                const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                    base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE) return;

                const DWORD import_rva = nt->OptionalHeader.DataDirectory[
                    IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
                if (!import_rva || import_rva >= image_size) return;

                auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                    base + import_rva);

                for (; desc->Name && desc->Name < image_size; ++desc)
                {
                    // Only process KERNEL32
                    const char* dll = reinterpret_cast<char*>(base + desc->Name);
                    if (_stricmp(dll, "KERNEL32.dll") != 0 &&
                        _stricmp(dll, "KERNEL32") != 0)
                        continue;

                    if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;

                    auto* oft = reinterpret_cast<IMAGE_THUNK_DATA*>(
                        base + desc->OriginalFirstThunk);
                    auto* ft = reinterpret_cast<IMAGE_THUNK_DATA*>(
                        base + desc->FirstThunk);

                    for (; oft->u1.AddressOfData; ++oft, ++ft)
                    {
                        if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
                        if (oft->u1.AddressOfData >= image_size)    continue;

                        const auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                            base + oft->u1.AddressOfData);

                        // Bounds-check the name pointer
                        const auto name_off =
                            reinterpret_cast<const uint8_t*>(ibn->Name) - base;
                        if (name_off >= static_cast<ptrdiff_t>(image_size)) continue;

                        if (strcmp(reinterpret_cast<const char*>(ibn->Name),
                            "IsDebuggerPresent") == 0)
                        {
                            const auto stub = reinterpret_cast<void*>(&debugger_stub);
                            mem_write(&ft->u1.Function, &stub, sizeof(stub));
                            return; // done
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        void patch_xlive()
        {
            const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA("xlive.dll"));
            if (!base) return;

            const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const size_t sz = nt->OptionalHeader.SizeOfImage;

            // Helper: scan and apply a patch to all matches.
            // patch_offset/patch_data/patch_len describe what to overwrite relative to match start.
            const auto patch_all = [&](const uint8_t* pat, size_t pat_len,
                size_t patch_offset, const uint8_t* patch_data, size_t patch_len)
                {
                    uint8_t* p = scan(base, sz, pat, pat_len);
                    while (p)
                    {
                        mem_write(p + patch_offset, patch_data, patch_len);
                        p = scan(p + 1, sz - (p - base) - 1, pat, pat_len);
                    }
                };

            static const uint8_t nop1 = 0x90;
            static const uint8_t nop2[2] = { 0x90, 0x90 };
            static const uint8_t nop6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
            static const uint8_t nop12[12] = {
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90
            };

            // Fix 1: redirect xlive's IsDebuggerPresent IAT entry to always return FALSE
            patch_isdebugger(base, sz);

            // Fix 2: NOP pointer corruption triggered when PEB.BeingDebugged == 1
            //   sub edi, 135h  (81 EF 35 01 00 00)
            //   sub esi, 1F620h (81 EE 20 F6 01 00)
            {
                static const uint8_t pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,
                    0x81,0xEE,0x20,0xF6,0x01,0x00
                };
                patch_all(pat, sizeof(pat), 0, nop12, 12);
            }

            // Fix 3A: NOP int 1 SEH traps (2 occurrences — sub_53D016 + sub_987D5C)
            //   and [TryLevel], 0 / int 1
            //   NOP: just the CD 01
            {
                static const uint8_t pat[] = { 0x83,0x65,0xFC,0x00, 0xCD,0x01 };
                patch_all(pat, sizeof(pat), 4, nop2, 2);
            }

            // Fix 3B: NOP int 3 SEH trap with magic SI/DI sentinel (sub_9883C3)
            //   mov si, 4647h / mov di, 4A4Dh / int 3
            //   NOP: just the CC
            {
                static const uint8_t pat[] = {
                    0x66,0xBE,0x47,0x46,
                    0x66,0xBF,0x4D,0x4A,
                    0xCC
                };
                patch_all(pat, sizeof(pat), 8, &nop1, 1);
            }

            // Fix 3C: NOP ud2 SEH trap (sub_53D016)
            //   mov [TryLevel], 2 / ud2
            //   NOP: just the 0F 0B
            {
                static const uint8_t pat[] = {
                    0xC7,0x45,0xFC,0x02,0x00,0x00,0x00,
                    0x0F,0x0B
                };
                patch_all(pat, sizeof(pat), 7, nop2, 2);
            }

            // Fix 4: NOP int 41h SEH traps (5 occurrences)
            //   mov ax, 4Fh / int 41h — invalid interrupt, SEH catches it
            //   NOP: just the CD 41 — AX stays 0x4F, falls through to "mov [var], ax"
            {
                static const uint8_t pat[] = { 0x66,0xB8,0x4F,0x00, 0xCD,0x41 };
                patch_all(pat, sizeof(pat), 4, nop2, 2);
            }

            // Fix 5: NOP divide-by-zero SEH traps (4 occurrences)
            //   xor eax, eax / div eax — intentional #DE, SEH catches it
            //   NOP: just the F7 F0 — EAX stays 0, falls through
            {
                static const uint8_t pat[] = { 0x33,0xC0, 0xF7,0xF0 };
                patch_all(pat, sizeof(pat), 2, nop2, 2);
            }

            // Fix 6: NOP POPFW trap-flag traps (4 occurrences)
            //   push word [ebp+var] / popfw — sets TF, causes single-step exception
            //   Scan first 3 bytes (66 FF 75), verify popfw (66 9D) at +4, NOP it
            {
                static const uint8_t pat[] = { 0x66,0xFF,0x75 };
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    if (p[4] == 0x66 && p[5] == 0x9D)
                        mem_write(p + 4, nop2, 2);
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            // Fix 7: NOP JB in sub_53D016 thread exit-code mask check
            //   xlive spawns threads using handle values as fn ptrs (dynamically mapped
            //   detection stubs). Thread exit codes carry the result, masked and compared.
            //   JB = "clean" -> returns 0 (correct not-detected state).
            //   Fall-through = "detected" -> computes kill function pointers.
            //   NOP the JB so execution always falls through to the clean return.
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,       // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00,       // cmp eax, 0C153B2h
                    0x0F,0x82,0xD6,0xFE,0xFF,0xFF   // jb loc_53D05B
                };
                patch_all(pat, sizeof(pat), 10, nop6, 6);
            }

            // Fix 8: JNB -> JMP in sub_4FF9D4 thread exit-code check
            //   Same thread detection, different structure. JNB jumps to success path.
            //   With debugger: masked result >= threshold -> JNB not taken -> error path.
            //   Change JNB (73) to JMP (EB), keeping the same offset byte (26).
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,   // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00,   // cmp eax, 0C153B2h
                    0x73,0x26                    // jnb +26h
                };
                static const uint8_t jmp = 0xEB;
                patch_all(pat, sizeof(pat), 10, &jmp, 1);
            }

            // Fix 9: NOP state XOR in XLiveInitialize path (sub_500A56)
            //   Reads PEB.BeingDebugged, stores it, then XORs xlive's state variable
            //   with 0x2B7 if nonzero — corrupting internal state -> XNetStartup fails.
            //   NOP the XOR (10 bytes). Without debugger, the preceding JZ already
            //   skips it; with debugger, our NOP prevents the corruption.
            {
                static const uint8_t pat[] = {
                    0x64,0xA1,0x18,0x00,0x00,0x00,  // mov eax, fs:[18h]
                    0x8B,0x40,0x30,                  // mov eax, [eax+30h]
                    0x0F,0xB6,0x40,0x02,             // movzx eax, [eax+2]
                    0x8B,0x4D,0xD4,                  // mov ecx, [hModule]
                    0x89,0x01,                        // mov [ecx], eax
                    0x39,0x55,0xD8,                  // cmp [var_28], edx
                    0x74,0x0A                         // jz +A
                };
                static const uint8_t nop10[10] = {
                    0x90,0x90,0x90,0x90,0x90,
                    0x90,0x90,0x90,0x90,0x90
                };
                patch_all(pat, sizeof(pat), sizeof(pat), nop10, 10);
            }
        }

        void clear_being_debugged()
        {
            // ONLY clear PEB.BeingDebugged — single byte write, safe from any thread.
            // Do NOT touch NtGlobalFlag or heap flags: those writes race with
            // HeapAlloc/HeapFree and corrupt the heap, crashing the game at runtime.
            const DWORD teb = __readfsdword(0x18);
            auto* peb = *reinterpret_cast<BYTE**>(teb + 0x30);
            peb[2] = 0; // BeingDebugged
        }

        void hook_ntqip()
        {
            const auto ntdll = GetModuleHandleA("ntdll.dll");
            if (!ntdll) return;
            const auto fn = GetProcAddress(ntdll, "NtQueryInformationProcess");
            if (!fn) return;
            ntqip_hook.create(fn, ntqip_stub);
        }

        DWORD WINAPI cleaner_thread(LPVOID)
        {
            while (true)
            {
                clear_being_debugged();
                Sleep(1);
            }
            return 0;
        }

    } // anonymous namespace

    void apply_early()
    {
        const auto xlive = GetModuleHandleA("xlive.dll");
        if (!xlive)
        {
            MessageBoxA(nullptr, "xlive.dll not loaded yet - patches not applied!", "xlive", MB_OK);
            return;
        }
        patch_xlive();
        clear_being_debugged();
        hook_ntqip();
        CloseHandle(CreateThread(nullptr, 0, cleaner_thread, nullptr, 0, nullptr));
    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            patch_xlive();
            clear_being_debugged();
        }
    };
}

REGISTER_COMPONENT(xlive::component)