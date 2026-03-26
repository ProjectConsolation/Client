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

        // Walk xlive.dll's import descriptors, find the KERNEL32 IsDebuggerPresent
        // IAT cell, and redirect it to our stub. Uses only the PE structures,
        // no hardcoded offsets, works on any xlive.dll version.
        void patch_isdebugger(uint8_t* xlive_base)
        {
            const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(xlive_base);
            const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                xlive_base + dos->e_lfanew);

            const DWORD import_rva = nt->OptionalHeader.DataDirectory[
                IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
            if (!import_rva) return;

            auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                xlive_base + import_rva);

            for (; desc->Name; ++desc)
            {
                const char* dll_name = reinterpret_cast<char*>(
                    xlive_base + desc->Name);

                // Only process KERNEL32 — that's where IsDebuggerPresent lives
                if (_stricmp(dll_name, "KERNEL32.dll") != 0 &&
                    _stricmp(dll_name, "KERNEL32") != 0)
                    continue;

                // OriginalFirstThunk has the hint/name entries
                // FirstThunk (IAT) has the resolved addresses at runtime
                auto* oft = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    xlive_base + desc->OriginalFirstThunk);
                auto* ft = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    xlive_base + desc->FirstThunk);

                for (; oft->u1.AddressOfData; ++oft, ++ft)
                {
                    // Skip ordinal imports
                    if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;

                    const auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        xlive_base + oft->u1.AddressOfData);

                    if (strcmp(reinterpret_cast<const char*>(ibn->Name),
                        "IsDebuggerPresent") == 0)
                    {
                        const auto stub = reinterpret_cast<void*>(&debugger_stub);
                        mem_write(&ft->u1.Function, &stub, sizeof(stub));
                        // Found and patched — done with this descriptor
                        return;
                    }
                }
            }
        }

        void patch_xlive()
        {
            const auto xlive_base = reinterpret_cast<uint8_t*>(
                GetModuleHandleA("xlive.dll"));
            if (!xlive_base) return;

            const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(xlive_base);
            const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                xlive_base + dos->e_lfanew);
            const size_t image_size = nt->OptionalHeader.SizeOfImage;

            // Fix 1: redirect IsDebuggerPresent in xlive's own IAT
            patch_isdebugger(xlive_base);

            // Fix 2: NOP pointer corruption after PEB.BeingDebugged check
            {
                static const uint8_t pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,  // sub edi, 135h
                    0x81,0xEE,0x20,0xF6,0x01,0x00   // sub esi, 1F620h
                };
                uint8_t* p = scan(xlive_base, image_size, pat, sizeof(pat));
                while (p)
                {
                    uint8_t nops[12]; memset(nops, 0x90, 12);
                    mem_write(p, nops, 12);
                    p = scan(p + 1, image_size - (p - xlive_base) - 1,
                        pat, sizeof(pat));
                }
            }

            // Fix 3: patch NtQIP mask check to always take safe path
            {
                static const uint8_t mask_pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,  // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00   // cmp eax, 0C153B2h
                };
                uint8_t* p = scan(xlive_base, image_size, mask_pat, sizeof(mask_pat));
                while (p)
                {
                    uint8_t* br = p + sizeof(mask_pat);
                    if (br[0] == 0x73)  // JNB short -> NOP
                    {
                        uint8_t nop2[2] = { 0x90, 0x90 };
                        mem_write(br, nop2, 2);
                    }
                    else if (br[0] == 0x0F && br[1] == 0x82)  // JB long -> JMP
                    {
                        uint8_t jmp[6] = { 0xE9, br[2], br[3], br[4], br[5], 0x90 };
                        mem_write(br, jmp, 6);
                    }
                    p = scan(p + 1, image_size - (p - xlive_base) - 1,
                        mask_pat, sizeof(mask_pat));
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