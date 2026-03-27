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
            static const uint8_t nop10[10] = { 0x90,0x90,0x90,0x90,0x90, 0x90,0x90,0x90,0x90,0x90 };
            static const uint8_t nop12[12] = { 0x90,0x90,0x90,0x90,0x90,0x90, 0x90,0x90,0x90,0x90,0x90,0x90 };
            static const uint8_t jmp_eb = 0xEB;  // shared short-JMP byte for all CC-check patches

            // Fix 1: redirect xlive's IsDebuggerPresent IAT entry to always return FALSE
            patch_isdebugger(base, sz);

            // Fix 2: NOP PEB.BeingDebugged pointer corruption
            //   sub edi, 135h / sub esi, 1F620h — only reached when BeingDebugged==1
            {
                static const uint8_t pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,
                    0x81,0xEE,0x20,0xF6,0x01,0x00
                };
                patch_all(pat, sizeof(pat), 0, nop12, 12);
            }

            // Fix 2B: bypass all INT3 breakpoint scans on sub_62D7A0
            //   VS patches sub_62D7A0[0] = 0xCC during thread creation. xlive checks this
            //   byte at 10 locations, corrupting function pointers or crashing on detection.
            //   Fix: JNZ -> JMP at each check so the "detected" branch is never taken.
            {
                // Primary checks — scan sub_62D7A0[0] directly (80 3x CC 75 xx)
                static const uint8_t cc_primary[][5] = {
                    {0x80,0x39,0xCC,0x75,0x1D},  // sub_53D016
                    {0x80,0x39,0xCC,0x75,0x05},  // sub_53A1DB: sub eax, 221h
                    {0x80,0x3A,0xCC,0x75,0x08},  // sub_53A1DB: add eax, 2BBh
                    {0x80,0x38,0xCC,0x75,0x10},  // sub_540EXX
                    {0x80,0x38,0xCC,0x75,0x04},  // sub_5493AF
                    {0x80,0x3A,0xCC,0x75,0x0A},  // sub_545CAB
                    {0x80,0x38,0xCC,0x75,0x0F},  // sub_8D43XX
                    {0x80,0x38,0xCC,0x75,0x0A},  // sub_989182
                    {0x80,0x3A,0xCC,0x75,0x07},  // sub_988E34
                    {0x80,0x3E,0xCC,0x75,0x0A},  // sub_989E34
                };
                for (const auto& pat : cc_primary)
                {
                    uint8_t* p = scan(base, sz, pat, 5);
                    while (p) { mem_write(p + 3, &jmp_eb, 1); p = scan(p + 1, sz - (p - base) - 1, pat, 5); }
                }

                // Secondary checks — compare decoded stack buffer against 0xCC.
                // These corrupt return values, write RET into live code, or shift values.
                struct { const uint8_t* pat; size_t len; size_t jnz_off; } cc_secondary[] = {
                    // sub_53D016: xor [var_44], 0DBh — corrupts return value, crashes at XOR'd addr
                    {(const uint8_t*)"\x80\x7D\xC7\xCC\x75\x07\x81\x75\xBC\xDB\x00\x00\x00", 13, 4},
                    // sub_5493AF: sar [var_44], 1Ah — right-shifts return value to near-zero
                    {(const uint8_t*)"\x80\x7D\xC3\xCC\x75\x04\xC1\x7D\xBC\x1A", 10, 4},
                    // sub_987D5C: and [var_144], 81h — masks detection state flags
                    {(const uint8_t*)"\x80\xBD\xC3\xFE\xFF\xFF\xCC\x75\x0A\x81\xA5\xBC\xFE\xFF\xFF\x81\x00\x00\x00", 19, 7},
                    // sub_988B71: add [ebx+4], 0C3h — writes RET opcode into live code
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x07\x81\x43\x04\xC3\x00\x00\x00", 13, 4},
                    // sub_988E34: add [Src], 110h — corrupts a string pointer
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x10\x81\x45\xA0\x10\x01\x00\x00", 13, 4},
                };
                for (const auto& e : cc_secondary)
                {
                    uint8_t* p = scan(base, sz, e.pat, e.len);
                    while (p) { mem_write(p + e.jnz_off, &jmp_eb, 1); p = scan(p + 1, sz - (p - base) - 1, e.pat, e.len); }
                }
            }

            // Fix 3A: NOP int 1 SEH traps (sub_53D016 + sub_987D5C)
            {
                static const uint8_t pat[] = { 0x83,0x65,0xFC,0x00, 0xCD,0x01 };
                patch_all(pat, sizeof(pat), 4, nop2, 2);
            }

            // Fix 3B: NOP int 3 SEH trap — magic SI/DI sentinel (sub_9883C3)
            {
                static const uint8_t pat[] = { 0x66,0xBE,0x47,0x46, 0x66,0xBF,0x4D,0x4A, 0xCC };
                patch_all(pat, sizeof(pat), 8, &nop1, 1);
            }

            // Fix 3C: NOP ud2 SEH trap (sub_53D016)
            {
                static const uint8_t pat[] = { 0xC7,0x45,0xFC,0x02,0x00,0x00,0x00, 0x0F,0x0B };
                patch_all(pat, sizeof(pat), 7, nop2, 2);
            }

            // Fix 4: NOP int 41h SEH traps (5 occurrences)
            //   mov ax, 4Fh / int 41h — AX stays 0x4F, falls through clean
            {
                static const uint8_t pat[] = { 0x66,0xB8,0x4F,0x00, 0xCD,0x41 };
                patch_all(pat, sizeof(pat), 4, nop2, 2);
            }

            // Fix 5: NOP divide-by-zero SEH traps (4 occurrences)
            //   xor eax, eax / div eax — EAX stays 0, falls through clean
            {
                static const uint8_t pat[] = { 0x33,0xC0, 0xF7,0xF0 };
                patch_all(pat, sizeof(pat), 2, nop2, 2);
            }

            // Fix 6: NOP POPFW trap-flag traps (4 occurrences)
            //   push word [ebp+var] / popfw — NOP the 66 9D
            {
                static const uint8_t pat[] = { 0x66,0xFF,0x75 };
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    if (p[4] == 0x66 && p[5] == 0x9D) mem_write(p + 4, nop2, 2);
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            // Fix 7: JB -> JMP in sub_53D016 thread exit-code mask check
            //   Forces always-clean early exit, bypasses sub_4D436C with bad state.
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,
                    0x3D,0xB2,0x53,0xC1,0x00,
                    0x0F,0x82,0xD6,0xFE,0xFF,0xFF
                };
                static const uint8_t jmp6[6] = { 0xE9,0xD6,0xFE,0xFF,0xFF, 0x90 };
                patch_all(pat, sizeof(pat), 10, jmp6, 6);
            }

            // Fix 8: JNB -> JMP in sub_4FF9D4 thread exit-code check
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,
                    0x3D,0xB2,0x53,0xC1,0x00,
                    0x73,0x26
                };
                patch_all(pat, sizeof(pat), 10, &jmp_eb, 1);
            }

            // Fix 9: NOP state XOR in XLiveInitialize (sub_500A56)
            //   Prevents BeingDebugged flag from corrupting xlive's init state variable.
            {
                static const uint8_t pat[] = {
                    0x64,0xA1,0x18,0x00,0x00,0x00,
                    0x8B,0x40,0x30,
                    0x0F,0xB6,0x40,0x02,
                    0x8B,0x4D,0xD4,
                    0x89,0x01,
                    0x39,0x55,0xD8,
                    0x74,0x0A
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