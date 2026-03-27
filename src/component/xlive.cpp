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
            const auto base = reinterpret_cast<uint8_t*>(
                GetModuleHandleA("xlive.dll"));
            if (!base) return;

            const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                base + dos->e_lfanew);
            const size_t sz = nt->OptionalHeader.SizeOfImage;

            patch_isdebugger(base, sz);

            // Fix 2: NOP pointer corruption (sub edi,135h / sub esi,1F620h)
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

            // Fix 3: NtQIP mask check inside sub_53D016 only.
            // The mask pattern (and eax, 0D9BB259Ch / cmp eax, 0C153B2h) appears 3 times:
            //   0x4ffbce - thread exit code check, JNB short. DO NOT PATCH - breaks success path.
            //   0x53d175 - NtQIP result in detection fn, JB long. PATCH - redirect to safe return.
            //   0x988139 - flag guard, JNB short. DO NOT PATCH - legitimate control flow.
            // Only the JB long (0F 82) variant is safe to patch.
            {
                static const uint8_t mask_pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,   // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00    // cmp eax, 0C153B2h
                };
                uint8_t* p = scan(base, sz, mask_pat, sizeof(mask_pat));
                while (p)
                {
                    uint8_t* br = p + sizeof(mask_pat);
                    // Only patch JB long (0F 82) -> JMP. Never patch JNB short (73).
                    if (br[0] == 0x0F && br[1] == 0x82)
                    {
                        uint8_t jmp[6] = { 0xE9, br[2], br[3], br[4], br[5], 0x90 };
                        mem_write(br, jmp, 6);
                    }
                    p = scan(p + 1, sz - (p - base) - 1, mask_pat, sizeof(mask_pat));
                }
            }

            // Fix 4: XLiveInitialize state corruption via PEB.BeingDebugged
            //   Called from xlive_5002 (XLiveInitialize). Reads BeingDebugged,
            //   stores it, then if nonzero: xor [state], 2B7h — corrupting xlive's
            //   internal state and causing both XLiveInitialize and XNetStartup to fail.
            //
            //   Pattern: full PEB read sequence leading up to the jz+xor
            //   64 A1 18 00 00 00  mov eax, fs:[18h]
            //   8B 40 30           mov eax, [eax+30h]
            //   0F B6 40 02        movzx eax, [eax+2]   ; BeingDebugged
            //   8B 4D D4           mov ecx, [hModule]
            //   89 01              mov [ecx], eax
            //   39 55 D8           cmp [var_28], edx
            //   74 0A              jz +0A               ; skip if not debugged
            //   81 35 xx xx xx xx  xor [state], 2B7h   <- NOP these 10 bytes
            //   B7 02 00 00
            {
                static const uint8_t peb_state_pat[] = {
                    0x64,0xA1,0x18,0x00,0x00,0x00,  // mov eax, fs:[18h]
                    0x8B,0x40,0x30,                  // mov eax, [eax+30h]
                    0x0F,0xB6,0x40,0x02,             // movzx eax, [eax+2]
                    0x8B,0x4D,0xD4,                  // mov ecx, [hModule]
                    0x89,0x01,                        // mov [ecx], eax
                    0x39,0x55,0xD8,                  // cmp [var_28], edx
                    0x74,0x0A                         // jz +0A
                };
                uint8_t* p = scan(base, sz, peb_state_pat, sizeof(peb_state_pat));
                while (p)
                {
                    // NOP the 10-byte xor immediately after the jz
                    uint8_t nops[10]; memset(nops, 0x90, 10);
                    mem_write(p + sizeof(peb_state_pat), nops, 10);
                    p = scan(p + 1, sz - (p - base) - 1, peb_state_pat, sizeof(peb_state_pat));
                }
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
        //patch_xlive();
        clear_being_debugged();
        //hook_ntqip();
       //CloseHandle(CreateThread(nullptr, 0, cleaner_thread, nullptr, 0, nullptr));
    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            //patch_xlive();
            //clear_being_debugged();
        }
    };
}

//REGISTER_COMPONENT(xlive::component)