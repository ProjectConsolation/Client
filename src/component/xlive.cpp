#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

namespace xlive
{
    namespace
    {
        utils::hook::detour ntqip_hook;

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

        void patch_isdebugger(uint8_t* base, size_t image_size)
        {
            __try
            {
                const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

                const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE) return;

                const DWORD import_rva = nt->OptionalHeader.DataDirectory[
                    IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
                if (!import_rva || import_rva >= image_size) return;

                auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + import_rva);

                for (; desc->Name && desc->Name < image_size; ++desc)
                {
                    const char* dll = reinterpret_cast<char*>(base + desc->Name);
                    if (_stricmp(dll, "KERNEL32.dll") != 0 &&
                        _stricmp(dll, "KERNEL32") != 0)
                        continue;

                    if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;

                    auto* oft = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
                    auto* ft = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

                    for (; oft->u1.AddressOfData; ++oft, ++ft)
                    {
                        if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
                        if (oft->u1.AddressOfData >= image_size)    continue;

                        const auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                            base + oft->u1.AddressOfData);
                        const auto name_off = reinterpret_cast<const uint8_t*>(ibn->Name) - base;
                        if (name_off >= static_cast<ptrdiff_t>(image_size)) continue;

                        if (strcmp(reinterpret_cast<const char*>(ibn->Name), "IsDebuggerPresent") == 0)
                        {
                            const auto stub = reinterpret_cast<void*>(&debugger_stub);
                            mem_write(&ft->u1.Function, &stub, sizeof(stub));
                            return;
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

            const auto patch_all = [&](const uint8_t* pat, size_t pat_len,
                size_t off, const uint8_t* data, size_t data_len)
                {
                    uint8_t* p = scan(base, sz, pat, pat_len);
                    while (p)
                    {
                        mem_write(p + off, data, data_len);
                        p = scan(p + 1, sz - (p - base) - 1, pat, pat_len);
                    }
                };

            static const uint8_t nop1 = 0x90;
            static const uint8_t nop2[2] = { 0x90, 0x90 };
            static const uint8_t nop10[10] = {
                0x90,0x90,0x90,0x90,0x90, 0x90,0x90,0x90,0x90,0x90
            };
            static const uint8_t nop12[12] = {
                0x90,0x90,0x90,0x90,0x90,0x90, 0x90,0x90,0x90,0x90,0x90,0x90
            };

            // Fix 1: redirect xlive's IsDebuggerPresent IAT entry to always return FALSE
            patch_isdebugger(base, sz);

            // Fix 2: NOP PEB.BeingDebugged pointer corruption
            //   sub edi, 135h / sub esi, 1F620h — only reached when BeingDebugged==1
            {
                static const uint8_t pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,   // sub edi, 135h
                    0x81,0xEE,0x20,0xF6,0x01,0x00    // sub esi, 1F620h
                };
                patch_all(pat, sizeof(pat), 0, nop12, 12);
            }

            // Fix 2B: bypass all INT3 breakpoint scans on sub_62D7A0
            //   xlive scans sub_62D7A0[0] for 0xCC (INT3) at 10 locations. VS patches that
            //   byte during thread creation/stepping, causing each check to fire and either
            //   corrupt function pointers, counters, or handles — eventually crashing.
            //   Fix: change every JNZ (75 xx) after the check to JMP (EB xx) so the
            //   "0xCC detected" branch is never taken. All patterns are unique in xlive.
            {
                static const struct { uint8_t pat[5]; } cc_checks[] = {
                    {{0x80,0x39,0xCC,0x75,0x1D}},  // sub_53D016:  jnz +1Dh
                    {{0x80,0x39,0xCC,0x75,0x05}},  // sub_53A1DB:  jnz +5  (sub eax,221h)
                    {{0x80,0x3A,0xCC,0x75,0x08}},  // sub_53A1DB:  jnz +8  (add eax,2BBh)
                    {{0x80,0x38,0xCC,0x75,0x10}},  // sub_540EXX:  jnz +10h
                    {{0x80,0x38,0xCC,0x75,0x04}},  // sub_5493AF:  jnz +4
                    {{0x80,0x3A,0xCC,0x75,0x0A}},  // sub_545CAB:  jnz +Ah
                    {{0x80,0x38,0xCC,0x75,0x0F}},  // sub_8D43XX:  jnz +Fh
                    {{0x80,0x38,0xCC,0x75,0x0A}},  // sub_989182:  jnz +Ah
                    {{0x80,0x3A,0xCC,0x75,0x07}},  // sub_988E34:  jnz +7
                    {{0x80,0x3E,0xCC,0x75,0x0A}},  // sub_989E34:  jnz +Ah
                };
                static const uint8_t jmp = 0xEB;
                for (const auto& entry : cc_checks)
                {
                    uint8_t* p = scan(base, sz, entry.pat, 5);
                    while (p)
                    {
                        mem_write(p + 3, &jmp, 1);  // 75 -> EB
                        p = scan(p + 1, sz - (p - base) - 1, entry.pat, 5);
                    }
                }
            }

            // Fix 3A: NOP int 1 SEH traps (sub_53D016 + sub_987D5C)
            //   and [TryLevel], 0 / int 1 — NOP just the CD 01
            {
                static const uint8_t pat[] = { 0x83,0x65,0xFC,0x00, 0xCD,0x01 };
                patch_all(pat, sizeof(pat), 4, nop2, 2);
            }

            // Fix 3B: NOP int 3 SEH trap — magic SI/DI sentinel (sub_9883C3)
            //   mov si, 4647h / mov di, 4A4Dh / int 3 — NOP just the CC
            {
                static const uint8_t pat[] = {
                    0x66,0xBE,0x47,0x46,
                    0x66,0xBF,0x4D,0x4A,
                    0xCC
                };
                patch_all(pat, sizeof(pat), 8, &nop1, 1);
            }

            // Fix 3C: NOP ud2 SEH trap (sub_53D016)
            //   mov [TryLevel], 2 / ud2 — NOP just the 0F 0B
            {
                static const uint8_t pat[] = {
                    0xC7,0x45,0xFC,0x02,0x00,0x00,0x00,
                    0x0F,0x0B
                };
                patch_all(pat, sizeof(pat), 7, nop2, 2);
            }

            // Fix 4: NOP int 41h SEH traps (5 occurrences)
            //   mov ax, 4Fh / int 41h — NOP just the CD 41
            //   AX stays 0x4F, falls through to "mov [var], ax" (clean state)
            {
                static const uint8_t pat[] = { 0x66,0xB8,0x4F,0x00, 0xCD,0x41 };
                patch_all(pat, sizeof(pat), 4, nop2, 2);
            }

            // Fix 5: NOP divide-by-zero SEH traps (4 occurrences)
            //   xor eax, eax / div eax — NOP just the F7 F0
            //   EAX stays 0, falls through to success path
            {
                static const uint8_t pat[] = { 0x33,0xC0, 0xF7,0xF0 };
                patch_all(pat, sizeof(pat), 2, nop2, 2);
            }

            // Fix 6: NOP POPFW trap-flag traps (4 occurrences)
            //   push word [ebp+var] / popfw — sets TF causing single-step exception
            //   Match first 3 bytes (66 FF 75), verify 66 9D at +4, NOP it
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

            // Fix 7: JB -> JMP in sub_53D016 thread exit-code mask check
            //   xlive creates threads via handle-value fn ptrs (dynamically mapped stubs).
            //   Exit codes carry detection results, masked and compared to a threshold.
            //   JB = "clean" path -> returns 0 (correct not-detected state).
            //   Fall-through = "detected" -> invokes sub_4D436C with corrupted state -> AV.
            //   Change JB (0F 82) to JMP (E9) so it always takes the clean early-exit path.
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,       // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00,       // cmp eax, 0C153B2h
                    0x0F,0x82,0xD6,0xFE,0xFF,0xFF   // jb loc_53D05B
                };
                static const uint8_t jmp6[6] = { 0xE9,0xD6,0xFE,0xFF,0xFF, 0x90 };
                patch_all(pat, sizeof(pat), 10, jmp6, 6);
            }

            // Fix 8: JNB -> JMP in sub_4FF9D4 thread exit-code check
            //   Same thread detection mechanism, different branch direction.
            //   JNB jumps to success path — with debugger it's not taken.
            //   Change JNB (73) to JMP (EB), same offset byte, to always succeed.
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
            //   Reads PEB.BeingDebugged into a struct, then XORs xlive's state var
            //   with 0x2B7 if nonzero — corrupting init state -> XNetStartup fails.
            //   NOP the XOR (10 bytes at pattern end). Safe without debugger since
            //   the preceding JZ already skips it when BeingDebugged==0.
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
                patch_all(pat, sizeof(pat), sizeof(pat), nop10, 10);
            }
        }

        void clear_being_debugged()
        {
            // Only clear PEB.BeingDebugged (byte at PEB+2). Single-byte write is
            // atomic on x86 and safe from any thread. Do NOT touch NtGlobalFlag or
            // heap flags — those race with HeapAlloc/HeapFree and corrupt the heap.
            const DWORD teb = __readfsdword(0x18);
            auto* peb = *reinterpret_cast<BYTE**>(teb + 0x30);
            peb[2] = 0;
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
            while (true) { clear_being_debugged(); Sleep(1); }
            return 0;
        }

    } // anonymous namespace

    void apply_early()
    {
        if (!GetModuleHandleA("xlive.dll"))
        {
            MessageBoxA(nullptr, "xlive.dll not loaded — patches skipped", "xlive", MB_OK);
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