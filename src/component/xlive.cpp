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

            // Fix 1: redirect xlive's IsDebuggerPresent IAT entry
            patch_isdebugger(base, sz);

            // Fix 2: NOP pointer corruption after PEB.BeingDebugged check
            {
                static const uint8_t corr_pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,
                    0x81,0xEE,0x20,0xF6,0x01,0x00
                };
                uint8_t* p = scan(base, sz, corr_pat, sizeof(corr_pat));
                while (p)
                {
                    uint8_t nops[12]; memset(nops, 0x90, 12);
                    mem_write(p, nops, 12);
                    p = scan(p + 1, sz - (p - base) - 1, corr_pat, sizeof(corr_pat));
                }
            }

            // Fix 3: NOP all SEH-based debugger traps (int 1 / int 3)
            //   xlive uses exception traps inside SEH frames to detect debuggers.
            //   Its SEH handlers catch the exception and set "clean" flags.
            //   With a debugger, VS intercepts the exception first -> flags never set -> detection.
            //   Fixing: NOP just the trap instruction so execution falls through directly
            //   to the "survived" path, identical to what the SEH handler produces.
            //
            //   A) int 1 in sub_53D016 + sub_987D5C (2 occurrences):
            //      83 65 FC 00  (and [TryLevel], 0)
            //      CD 01        (int 1)    <- NOP these 2 bytes
            {
                static const uint8_t pat[] = { 0x83,0x65,0xFC,0x00,0xCD,0x01 };
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    uint8_t nop2[2] = { 0x90, 0x90 };
                    mem_write(p + 4, nop2, 2);  // NOP just the CD 01
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            //   B) int 3 with magic SI/DI values in sub_9883C3:
            //      66 BE 47 46  (mov si, 4647h)
            //      66 BF 4D 4A  (mov di, 4A4Dh)
            //      CC           (int 3)    <- NOP this 1 byte
            {
                static const uint8_t pat[] = {
                    0x66,0xBE,0x47,0x46,
                    0x66,0xBF,0x4D,0x4A,
                    0xCC
                };
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    uint8_t nop = 0x90;
                    mem_write(p + 8, &nop, 1);  // NOP just the CC
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            //   C) int 3 in sub_9887CD:
            //      C7 45 FC 02 00 00 00  (mov [TryLevel], 2)
            //      CC                    (int 3)    <- NOP this 1 byte
            {
                static const uint8_t pat[] = {
                    0xC7,0x45,0xFC,0x02,0x00,0x00,0x00,
                    0xCC
                };
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    uint8_t nop = 0x90;
                    mem_write(p + 7, &nop, 1);  // NOP just the CC
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            // NOTE: mask_pat (NtQIP thread check) removed — sub_53D016 is a function
            // pointer resolver, not just anti-debug. Patching its return path nulls
            // xlive's entire function dispatch table causing crashes.
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