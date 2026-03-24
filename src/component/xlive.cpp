#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

namespace xlive
{
    namespace
    {
        utils::hook::detour ntqip_hook;

        LONG __stdcall ntqip_stub(
            HANDLE hProcess,
            UINT   infoClass,
            PVOID  pInfo,
            ULONG  infoLen,
            PULONG pRetLen)
        {
            if (infoClass == 7) // ProcessDebugPort
            {
                if (pInfo && infoLen >= sizeof(ULONG_PTR))
                    *static_cast<ULONG_PTR*>(pInfo) = 0;
                if (pRetLen) *pRetLen = sizeof(ULONG_PTR);
                return 0;
            }
            if (infoClass == 0x1E) // ProcessDebugObjectHandle
            {
                if (pInfo && infoLen >= sizeof(HANDLE))
                    *static_cast<HANDLE*>(pInfo) = nullptr;
                return static_cast<LONG>(0xC0000353);
            }
            return static_cast<LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG, PULONG)>(
                ntqip_hook.get_original())(hProcess, infoClass, pInfo, infoLen, pRetLen);
        }

        void clear_being_debugged()
        {
            // xlive reads PEB.BeingDebugged directly via FS:[18h]->TEB->[+30h]->PEB->[+2]
            const DWORD teb = __readfsdword(0x18);
            auto* peb = *reinterpret_cast<BYTE**>(teb + 0x30);
            peb[2] = 0;
        }

        void hook_ntqip()
        {
            const auto ntdll = GetModuleHandleA("ntdll.dll");
            if (!ntdll) return;
            auto* fn = GetProcAddress(ntdll, "NtQueryInformationProcess");
            if (!fn) return;
            ntqip_hook.create(fn, ntqip_stub);
        }

        // xlive uses SEH-based detection (i believe): deliberately writes to near-null addresses
        // to trigger an AV. The SEH handler catches the AV when no debugger is present.
        // With a debugger, VS intercepts the first-chance AV before the SEH handler,
        // so xlive detects the debugger and corrupts state / fails to init networking.
        //
        // so i map those pages as writable so the writes succeed silently —
        // no AV is raised, the SEH trick never fires, xlive inits normally.
        //
        // Known kill-write targets observed:
        //   0x00007978 (page 0x7000) - crash at xlive 64A6A877
        //   0x0000F319 (page 0xF000) - crash at xlive 64FC66C6, 64FB32AE (ASLR)
        void map_kill_pages()
        {
            // Map every page in the 0x1000-0x10000 range (the "null region" xlive
            // uses for all its sentinel writes). This covers all known targets and
            // any others xlive might add in different code paths.
            // Each page is 4KB; 0x1000 to 0x10000 = 60KB total — negligible cost.
            for (DWORD page = 0x1000; page < 0x10000; page += 0x1000)
            {
                // Skip pages that are already committed (e.g. if something else
                // mapped them). VirtualAlloc with MEM_RESERVE|MEM_COMMIT on an
                // already-committed page returns null — that's fine, just skip it.
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi))
                    && mbi.State == MEM_COMMIT)
                    continue;

                VirtualAlloc(reinterpret_cast<void*>(page), 0x1000,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            }
        }

    }

    void apply_early()
    {
        map_kill_pages();       // Fix C: map near-null pages so SEH kill-writes succeed
        clear_being_debugged(); // Fix A: zero PEB.BeingDebugged
        hook_ntqip();           // Fix B: NtQIP ProcessDebugPort -> 0
    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            clear_being_debugged();
        }
    };
}

REGISTER_COMPONENT(xlive::component)