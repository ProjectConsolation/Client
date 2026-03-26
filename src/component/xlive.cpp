#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

namespace xlive
{
    namespace
    {
        utils::hook::detour ntqip_hook;

        LONG __stdcall ntqip_stub(HANDLE hProcess, UINT infoClass,
            PVOID pInfo, ULONG infoLen, PULONG pRetLen)
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
            const DWORD teb = __readfsdword(0x18);
            auto* peb = *reinterpret_cast<BYTE**>(teb + 0x30);
            peb[2] = 0; // BeingDebugged

            // NtGlobalFlag (PEB+0x68): debugger sets bits 0x70
            *reinterpret_cast<DWORD*>(peb + 0x68) &= ~0x70u;

            // Default heap flags (PEB+0x18 = ProcessHeap)
            // Debugger sets 0x50 in heap header+0x40/+0x44
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
            auto* fn = GetProcAddress(ntdll, "NtQueryInformationProcess");
            if (!fn) return;
            ntqip_hook.create(fn, ntqip_stub);
        }

    } // anonymous namespace

    void apply_early()
    {
        // NOTE: map_kill_pages() removed. The writes to 0x7978 / 0xF319 are
        // inside xlive SEH frames -- if the write succeeds, xlive follows the
        // wrong post-SEH path and breaks its init. Let them AV so the SEH
        // handler runs correctly. Configure VS Exception Settings to not break
        // on 0xC0000005 (Access Violation) so these pass through to xlive's
        // SEH handler transparently.
        clear_being_debugged();
        hook_ntqip();
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