#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

// ============================================================================
// xlive.cpp — complete xlive.dll anti-debug bypass via in-memory patching
//
// Scans the loaded xlive.dll image for known byte patterns and patches them
// directly. Works regardless of load order or ASLR — no timing dependency.
//
// Patches applied:
//   1. PEB corruption (sub edi,135h / sub esi,1F620h) — NOPed
//   2. SEH kill-write to 0x7978 — NOPed
//   3. SEH kill-write to 0xF319 (byte and dword variants) — NOPed
//   4. INT 1 SEH single-step trick — NOPed
//   5. NtQIP result mask check JNB/JB — patched to always take safe path
//   6. NtQueryInformationProcess hook — ProcessDebugPort/ObjectHandle zeroed
//   7. PEB.BeingDebugged + NtGlobalFlag + heap flags cleaner thread
// ============================================================================

namespace xlive
{
    namespace
    {
        utils::hook::detour ntqip_hook;

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

        // Scan [base, base+size) for pattern, returns pointer to first match or null.
        uint8_t* scan(uint8_t* base, size_t size,
            const uint8_t* pat, size_t pat_len)
        {
            if (!pat_len || size < pat_len) return nullptr;
            for (size_t i = 0, lim = size - pat_len; i <= lim; ++i)
                if (!memcmp(base + i, pat, pat_len))
                    return base + i;
            return nullptr;
        }

        // Scan and NOP all occurrences of a pattern.
        void nop_pattern(uint8_t* base, size_t image_size,
            const uint8_t* pat, size_t pat_len,
            size_t nop_len = 0)
        {
            if (!nop_len) nop_len = pat_len;
            uint8_t* p = scan(base, image_size, pat, pat_len);
            while (p)
            {
                uint8_t nops[64] = {};
                memset(nops, 0x90, nop_len);
                mem_write(p, nops, nop_len);
                p = scan(p + 1, image_size - (p - base) - 1, pat, pat_len);
            }
        }

        void patch_xlive()
        {
            const auto base = reinterpret_cast<uint8_t*>(
                GetModuleHandleA("xlive.dll"));
            if (!base) return;

            const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                base + dos->e_lfanew);
            const size_t sz = nt->OptionalHeader.SizeOfImage;

            // ------------------------------------------------------------------
            // 1. PEB.BeingDebugged -> pointer corruption
            //    sub edi, 135h  (81 EF 35 01 00 00)
            //    sub esi, 1F620h (81 EE 20 F6 01 00)
            //    NOP both (12 bytes)
            // ------------------------------------------------------------------
            {
                static const uint8_t pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,  // sub edi, 135h
                    0x81,0xEE,0x20,0xF6,0x01,0x00   // sub esi, 1F620h
                };
                nop_pattern(base, sz, pat, sizeof(pat));
            }

            // ------------------------------------------------------------------
            // 2. SEH kill-write to 0x7978
            //    3E C7 05 78 79 00 00 0B 95 71 4C  (11 bytes, DS prefix + MOV)
            // ------------------------------------------------------------------
            {
                static const uint8_t pat[] = {
                    0x3E,0xC7,0x05,0x78,0x79,0x00,0x00,
                    0x0B,0x95,0x71,0x4C
                };
                nop_pattern(base, sz, pat, sizeof(pat));
            }

            // ------------------------------------------------------------------
            // 3. SEH kill-write to 0xF319 — byte variant
            //    C6 05 19 F3 00 00 99  (7 bytes)
            // ------------------------------------------------------------------
            {
                static const uint8_t pat[] = {
                    0xC6,0x05,0x19,0xF3,0x00,0x00,0x99
                };
                nop_pattern(base, sz, pat, sizeof(pat));
            }

            // ------------------------------------------------------------------
            // 3b. SEH kill-write to 0xF319 — dword variant
            //     C7 05 19 F3 00 00 xx xx xx xx  (10 bytes, value unknown)
            //     Only match the first 6 bytes, NOP 10
            // ------------------------------------------------------------------
            {
                static const uint8_t pat[] = {
                    0xC7,0x05,0x19,0xF3,0x00,0x00
                };
                nop_pattern(base, sz, pat, sizeof(pat), 10);
            }

            // ------------------------------------------------------------------
            // 4. INT 1 SEH single-step trick
            //    Pattern: SEH TryLevel clear + INT 1
            //    83 65 FC 00  (and [TryLevel], 0)
            //    CD 01        (int 1)
            //    NOP just the CD 01 (2 bytes at offset +4)
            // ------------------------------------------------------------------
            {
                static const uint8_t pat[] = {
                    0x83,0x65,0xFC,0x00,  // and [ebp+ms_exc.TryLevel], 0
                    0xCD,0x01             // int 1
                };
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    // NOP only the int 1 (bytes 4-5), keep the SEH setup
                    static const uint8_t nop2[2] = { 0x90, 0x90 };
                    mem_write(p + 4, nop2, 2);
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            // ------------------------------------------------------------------
            // 5. NtQIP result mask check
            //    25 9C 25 BB D9  (and eax, 0D9BB259Ch)
            //    3D B2 53 C1 00  (cmp eax, 0C153B2h)
            //    Then one of:
            //      73 xx         (JNB short, 2 bytes) -> NOP 2
            //      0F 82 xx xx xx xx  (JB long, 6 bytes) -> E9 xx xx xx xx 90
            // ------------------------------------------------------------------
            {
                static const uint8_t mask_pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,  // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00   // cmp eax, 0C153B2h
                };
                uint8_t* p = scan(base, sz, mask_pat, sizeof(mask_pat));
                while (p)
                {
                    uint8_t* branch = p + sizeof(mask_pat);
                    if (branch[0] == 0x73) // JNB short -> NOP 2
                    {
                        static const uint8_t nop2[2] = { 0x90, 0x90 };
                        mem_write(branch, nop2, 2);
                    }
                    else if (branch[0] == 0x0F && branch[1] == 0x82) // JB long -> JMP
                    {
                        // Change JB (0F 82 xx xx xx xx) to JMP (E9 xx xx xx xx 90)
                        // Keeps the same relative offset so it still jumps to safe path
                        uint8_t jmp[6] = { 0xE9, branch[2], branch[3],
                                          branch[4], branch[5], 0x90 };
                        mem_write(branch, jmp, 6);
                    }
                    p = scan(p + 1, sz - (p - base) - 1, mask_pat, sizeof(mask_pat));
                }
            }
        }

        void clear_being_debugged()
        {
            const DWORD teb = __readfsdword(0x18);
            auto* peb = *reinterpret_cast<BYTE**>(teb + 0x30);
            peb[2] = 0;
            *reinterpret_cast<DWORD*>(peb + 0x68) &= ~0x70u;
            const auto heap = *reinterpret_cast<BYTE**>(peb + 0x18);
            if (heap)
            {
                *reinterpret_cast<DWORD*>(heap + 0x40) &= ~0x50u;
                *reinterpret_cast<DWORD*>(heap + 0x44) &= ~0x40u;
            }
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
        patch_xlive();          // Patch all known detection branches in xlive.dll
        clear_being_debugged(); // Clear PEB flags right now
        hook_ntqip();           // Hook NtQIP for ProcessDebugPort queries
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