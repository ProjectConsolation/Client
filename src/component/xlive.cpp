/*
 * xlive.cpp — GFWL (Games for Windows Live) anti-debug bypass
 *
 * The retail xlive.dll wraps its public API inside a protection layer that
 * detects debuggers and corrupts internal state on detection. This component
 * patches all known checks so VS can attach freely.
 *
 * ── How xlive's thread-based detection works ─────────────────────────────────
 *
 * xlive (2009) uses two independent detection mechanisms:
 *
 *   Mechanism A — hardware breakpoint detction (sub_4F8A59):
 *     DuplicateHandle(current_thread) -> TargetHandle
 *     CreateThread(lpStart=sub_5F75F8, lpParam=TargetHandle)
 *     sub_5F75F8: SuspendThread(param) -> GetThreadContext -> checks Dr7
 *       Dr7 == 0 (no HW BPs): exit code = 0xF3B02C90
 *       Dr7 != 0 (HW BPs set): exit code = 1
 *     Parent reads exit code, applies mask 0xD9BB259C / threshold 0xC153B2.
 *
 *   Mechanism B — low-memory stub execution (sub_5072BB):
 *     DuplicateHandle(current_thread) -> TargetHandle (a small integer like 0x968)
 *     CreateThread(lpStart=TargetHandle, ...) — executes code AT the handle address
 *     This relied on xlive pre-mapping executable stubs at handle-range addresses
 *     using VirtualAlloc on Windows XP/Vista/7, where the first 64 KB was
 *     allocatable. On Windows 10/11 this is impossible — the kernel permanently
 *     reserves the first 64 KB — so CreateThread crashes with AV at 0x968 etc.
 *
 * ── Why naive byte patches don't fix it ──────────────────────────────────────
 *
 * The "detcted" fall-through path in sub_5072BB leads to sub_8DC4BC which
 * performs the actual low-mmeory setup. Patching the branch that skips to the
 * early return (which we previously called Fix 7/8/11) also skips sub_8DC4BC,
 * leaving the thread stub unmapped — same crash, different address each run.
 *
 * ── The correct fix ───────────────────────────────────────────────────────────
 *
 * Hook CreateThread in xlive's own IAT. When lpStartAddress < 0x10000 (a
 * handle-value execution attempt), skip the thread entirely and return a fake
 * HANDLE whose subsequent WaitForSingleObject / GetExitCodeThread calls return
 * a clean exit code (0xF3B02C90) that keeps xlive's state machine healthy.
 *
 * All other CreateThread calls (lpStart >= 0x10000) are forwarded normally.
 *
 * Ordinal reference (from xlive.lib):
 *   xlive_5000 = XLiveInitialize
 *   xlive_5001 = _XLiveInput@4
 *   xlive_5002 = _XLiveRender@0
 *   xlive_5003 = _XLiveUnInitialize@0
 *   xlive_5251 = _XCloseHandle@4
 *   xlive_5297 = _XLiveInitializeEx@8
 *   xlive_51   = _XNetStartup@4
 *   xlive_52   = _XNetCleanup@0
 */

#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

namespace xlive
{
    namespace
    {
        utils::hook::detour ntqip_hook;

        // Replaces xlive's IAT slot for IsDebuggerPresent — always returns FALSE.
        static BOOL WINAPI IsDebuggerPresent_stub() { return FALSE; }

        // Hooks NtQueryInformationProcess to hide the debugger from xlive's
        // explicit kernel queries (ProcessDebugPort=7, ProcessDebugObjectHandle=0x1E).
        LONG __stdcall NtQueryInformationProcess_hook(HANDLE hProcess, UINT infoClass,
            PVOID pInfo, ULONG infoLen,
            PULONG pRetLen)
        {
            if (infoClass == 7)  // ProcessDebugPort
            {
                if (pInfo && infoLen >= sizeof(ULONG_PTR))
                    *static_cast<ULONG_PTR*>(pInfo) = 0;
                if (pRetLen) *pRetLen = sizeof(ULONG_PTR);
                return 0;
            }
            if (infoClass == 0x1E)  // ProcessDebugObjectHandle
            {
                if (pInfo && infoLen >= sizeof(HANDLE))
                    *static_cast<HANDLE*>(pInfo) = nullptr;
                return static_cast<LONG>(0xC0000353);  // STATUS_PORT_NOT_SET
            }
            return static_cast<LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG, PULONG)>(
                ntqip_hook.get_original())(hProcess, infoClass, pInfo, infoLen, pRetLen);
        }

        // -------------------------------------------------------------------------
        // Low-memory thread intercept
        //
        // xlive's Mechanism B creates threads with lpStartAddress = a handle value
        // (typically < 0x10000). On Windows XP/Vista/7 these pages were allocatable;
        // on Windows 10/11 the kernel owns the first 64 KB and attempting to execute
        // there crashes with AV regardless of any protection bypass.
        //
        // This stub intercepts those calls and returns a fake HANDLE backed by a
        // real event object. WaitForSingleObject on it returns WAIT_OBJECT_0
        // immediately, and GetExitCodeThread returns 0xF3B02C90 — the "no hardware
        // breakpoints" clean value that xlive's mask checks expect.
        //
        // All CreateThread calls with lpStartAddress >= 0x10000 are forwarded
        // unchanged so xlive's legitimate threading still works.

        // Sentinel exit code: the value sub_5F75F8 returns when Dr7 == 0.
        // Both xlive mask checks (0xD9BB259C and 0xE5937CB2) produce results
        // below their respective thresholds with this value, satisfying xlive.
        static constexpr DWORD CLEAN_EXIT_CODE = 0xF3B02C90;

        // A thread-local "fake handle" pool. xlive holds at most a handful of
        // these concurrently so a fixed-size ring is sufficient.
        static constexpr size_t FAKE_POOL_SIZE = 32;
        struct FakeThread
        {
            HANDLE event = nullptr; // manual-reset event, pre-signalled
            DWORD  exit_code = CLEAN_EXIT_CODE;
            bool   active = false;
        };
        static FakeThread fake_pool[FAKE_POOL_SIZE];
        static CRITICAL_SECTION fake_pool_cs;
        static bool fake_pool_init = false;

        static void ensure_fake_pool()
        {
            if (fake_pool_init) return;
            InitializeCriticalSection(&fake_pool_cs);
            fake_pool_init = true;
        }

        // Encode a FakeThread* as a pseudo-HANDLE using a sentinel tag bit.
        // Real handles are always multiples of 4 and have bits 0-1 clear.
        // We use the lowest bit to tag our fake handles; real code never sees
        // them except through our hooked GetExitCodeThread/WaitForSingleObject.
        static HANDLE encode_fake(FakeThread* ft)
        {
            return reinterpret_cast<HANDLE>(
                reinterpret_cast<uintptr_t>(ft) | 1u);
        }
        static FakeThread* decode_fake(HANDLE h)
        {
            const auto v = reinterpret_cast<uintptr_t>(h);
            if ((v & 1u) == 0) return nullptr;
            auto* ft = reinterpret_cast<FakeThread*>(v & ~uintptr_t(1));
            // Bounds-check against pool
            if (ft < fake_pool || ft >= fake_pool + FAKE_POOL_SIZE) return nullptr;
            return ft->active ? ft : nullptr;
        }

        static HANDLE alloc_fake_thread()
        {
            ensure_fake_pool();
            EnterCriticalSection(&fake_pool_cs);
            FakeThread* ft = nullptr;
            for (auto& slot : fake_pool)
            {
                if (!slot.active) { ft = &slot; break; }
            }
            if (!ft)
            {
                LeaveCriticalSection(&fake_pool_cs);
                return nullptr;
            }
            if (!ft->event)
                ft->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ft->exit_code = CLEAN_EXIT_CODE;
            ft->active = true;
            SetEvent(ft->event); // pre-signalled so WaitForSingleObject returns immediately
            LeaveCriticalSection(&fake_pool_cs);
            return encode_fake(ft);
        }

        static void free_fake_thread(FakeThread* ft)
        {
            EnterCriticalSection(&fake_pool_cs);
            ResetEvent(ft->event);
            ft->active = false;
            LeaveCriticalSection(&fake_pool_cs);
        }

        // Hooked CreateThread — intercepted in xlive's own IAT.
        static HANDLE WINAPI CreateThread_hook(
            LPSECURITY_ATTRIBUTES sec, SIZE_T stack,
            LPTHREAD_START_ROUTINE start, LPVOID param,
            DWORD flags, LPDWORD tid)
        {
            if (reinterpret_cast<uintptr_t>(start) < 0x10000u)
            {
                // Low-memory stub thread — cannot execute on Win10/11.
                // Return a fake signalled handle; callers see immediate completion
                // with the clean exit code.
                if (tid) *tid = 0;
                HANDLE h = alloc_fake_thread();
                return h ? h : INVALID_HANDLE_VALUE;
            }
            return CreateThread(sec, stack, start, param, flags, tid);
        }

        // Hooked WaitForSingleObject — passes fake handles without blocking.
        static DWORD WINAPI WaitForSingleObject_hook(HANDLE h, DWORD timeout)
        {
            if (FakeThread* ft = decode_fake(h))
                return WAIT_OBJECT_0;
            return WaitForSingleObject(h, timeout);
        }

        // Hooked GetExitCodeThread — returns the clean exit code for fake handles.
        static BOOL WINAPI GetExitCodeThread_hook(HANDLE h, LPDWORD exit_code)
        {
            if (FakeThread* ft = decode_fake(h))
            {
                if (exit_code) *exit_code = ft->exit_code;
                return TRUE;
            }
            return GetExitCodeThread(h, exit_code);
        }

        // Hooked CloseHandle — recycles fake handles silently.
        static BOOL WINAPI CloseHandle_hook(HANDLE h)
        {
            if (FakeThread* ft = decode_fake(h))
            {
                free_fake_thread(ft);
                return TRUE;
            }
            return CloseHandle(h);
        }

        // -------------------------------------------------------------------------
        // Patch helpers

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

        // -------------------------------------------------------------------------
        // Fix 1 — redirect IsDebuggerPresent in xlive's own IAT

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

                        const char* fn_name = reinterpret_cast<const char*>(ibn->Name);
                        void* stub = nullptr;

                        if (strcmp(fn_name, "IsDebuggerPresent") == 0) stub = reinterpret_cast<void*>(&IsDebuggerPresent_stub);
                        else if (strcmp(fn_name, "CreateThread") == 0) stub = reinterpret_cast<void*>(&CreateThread_hook);
                        else if (strcmp(fn_name, "WaitForSingleObject") == 0) stub = reinterpret_cast<void*>(&WaitForSingleObject_hook);
                        else if (strcmp(fn_name, "GetExitCodeThread") == 0) stub = reinterpret_cast<void*>(&GetExitCodeThread_hook);
                        else if (strcmp(fn_name, "CloseHandle") == 0) stub = reinterpret_cast<void*>(&CloseHandle_hook);

                        if (stub)
                            mem_write(&ft->u1.Function, &stub, sizeof(stub));
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // -------------------------------------------------------------------------

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

            static const uint8_t NOP1 = 0x90;
            static const uint8_t NOP2[2] = { 0x90, 0x90 };
            static const uint8_t NOP10[10] = { 0x90,0x90,0x90,0x90,0x90, 0x90,0x90,0x90,0x90,0x90 };
            static const uint8_t NOP12[12] = { 0x90,0x90,0x90,0x90,0x90,0x90, 0x90,0x90,0x90,0x90,0x90,0x90 };
            static const uint8_t JMP_SHORT = 0xEB;

            // -------------------------------------------------------------------
            // Fix 1: IAT hooks — IsDebuggerPresent + CreateThread intercept
            //
            // Redirects xlive's own IAT slots. CreateThread_hook intercepts the
            // low-memory thread creation (Mechanism B) that crashes on Win10/11.
            // WaitForSingleObject/GetExitCodeThread/CloseHandle hooks complete
            // the illusion so xlive's state machine sees a "clean" thread result.
            patch_isdebugger(base, sz);

            // -------------------------------------------------------------------
            // Fix 2: NOP PEB.BeingDebugged pointer corruption
            //
            // When PEB.BeingDebugged == 1, xlive corrupts two pointer registers
            // used for subsequent function dispatch:
            //   sub edi, 135h    ; corrupts dispatch table offset
            //   sub esi, 1F620h  ; corrupts module base pointer
            // Both are NOPed so dispatch works normally with a debugger attached.
            {
                static const uint8_t pat[] = {
                    0x81,0xEF,0x35,0x01,0x00,0x00,   // sub edi, 135h
                    0x81,0xEE,0x20,0xF6,0x01,0x00     // sub esi, 1F620h
                };
                patch_all(pat, sizeof(pat), 0, NOP12, 12);
            }

            // -------------------------------------------------------------------
            // Fix 2B: bypass INT3 breakpoint scans (Mechanism A — 0xCC checks)
            //
            // xlive checks whether the first byte of the thread-inspect function
            // (sub_5F75F8) is 0xCC (an INT3 that VS injects on thread creation)
            // at 14+ call sites. Detection either corrupts a function pointer
            // (crash at mangled address) or causes early exit (stub unmapped).
            // Fix: change every JNZ following the 0xCC comparison to JMP.
            {
                // Primary checks — 5-byte form: cmp [reg], 0CCh / jnz +xx
                static const uint8_t cc_primary[][5] = {
                    {0x80,0x39,0xCC,0x75,0x1D},
                    {0x80,0x39,0xCC,0x75,0x05},
                    {0x80,0x3A,0xCC,0x75,0x08},
                    {0x80,0x38,0xCC,0x75,0x10},
                    {0x80,0x38,0xCC,0x75,0x04},
                    {0x80,0x3A,0xCC,0x75,0x0A},
                    {0x80,0x38,0xCC,0x75,0x0F},
                    {0x80,0x38,0xCC,0x75,0x0A},
                    {0x80,0x3A,0xCC,0x75,0x07},
                    {0x80,0x3E,0xCC,0x75,0x0A},
                };
                for (const auto& pat : cc_primary)
                {
                    uint8_t* p = scan(base, sz, pat, 5);
                    while (p)
                    {
                        mem_write(p + 3, &JMP_SHORT, 1);
                        p = scan(p + 1, sz - (p - base) - 1, pat, 5);
                    }
                }

                // Variable-length checks — JNZ offset and pattern length differ.
                struct CcPatch { const uint8_t* pat; size_t len; size_t jnz_off; };
                static const CcPatch cc_var[] = {
                    {(const uint8_t*)"\x80\x7D\xC7\xCC\x75\x07\x81\x75\xBC\xDB\x00\x00\x00", 13, 4},
                    {(const uint8_t*)"\x80\x7D\xC3\xCC\x75\x04\xC1\x7D\xBC\x1A",             10, 4},
                    {(const uint8_t*)"\x80\xBD\xC3\xFE\xFF\xFF\xCC\x75\x0A\x81\xA5\xBC\xFE\xFF\xFF\x81\x00\x00\x00", 19, 7},
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x07\x81\x43\x04\xC3\x00\x00\x00", 13, 4},
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x10\x81\x45\xA0\x10\x01\x00\x00", 13, 4},
                    {(const uint8_t*)"\x80\xBD\xC3\xFE\xFF\xFF\xCC\x75\x1D",  9, 7},
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x1B",              6, 4},
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x1C",              6, 4},
                    {(const uint8_t*)"\x80\xBD\xDB\xFE\xFF\xFF\xCC\x75\x18",  9, 7},
                };
                for (const auto& e : cc_var)
                {
                    uint8_t* p = scan(base, sz, e.pat, e.len);
                    while (p)
                    {
                        mem_write(p + e.jnz_off, &JMP_SHORT, 1);
                        p = scan(p + 1, sz - (p - base) - 1, e.pat, e.len);
                    }
                }
            }

            // -------------------------------------------------------------------
            // Fix 3A: NOP int 1 SEH traps
            {
                static const uint8_t pat[] = {
                    0x83,0x65,0xFC,0x00,  // and [TryLevel], 0
                    0xCD,0x01             // int 1  <- NOP
                };
                patch_all(pat, sizeof(pat), 4, NOP2, 2);
            }

            // Fix 3B: NOP int 3 SEH trap — magic SI/DI sentinel
            {
                static const uint8_t pat[] = {
                    0x66,0xBE,0x47,0x46,  // mov si, 4647h
                    0x66,0xBF,0x4D,0x4A,  // mov di, 4A4Dh
                    0xCC                  // int 3  <- NOP
                };
                patch_all(pat, sizeof(pat), 8, &NOP1, 1);
            }

            // Fix 3C: NOP ud2 SEH trap
            {
                static const uint8_t pat[] = {
                    0xC7,0x45,0xFC,0x02,0x00,0x00,0x00,  // mov [TryLevel], 2
                    0x0F,0x0B                              // ud2  <- NOP
                };
                patch_all(pat, sizeof(pat), 7, NOP2, 2);
            }

            // -------------------------------------------------------------------
            // Fix 4: NOP int 41h SEH traps (5 occurrences)
            {
                static const uint8_t pat[] = {
                    0x66,0xB8,0x4F,0x00,  // mov ax, 4Fh
                    0xCD,0x41             // int 41h  <- NOP
                };
                patch_all(pat, sizeof(pat), 4, NOP2, 2);
            }

            // -------------------------------------------------------------------
            // Fix 5: NOP divide-by-zero SEH traps (4 occurrences)
            {
                static const uint8_t pat[] = {
                    0x33,0xC0,  // xor eax, eax
                    0xF7,0xF0   // div eax  <- NOP
                };
                patch_all(pat, sizeof(pat), 2, NOP2, 2);
            }

            // -------------------------------------------------------------------
            // Fix 6: NOP POPFW trap-flag traps (4 occurrences)
            {
                static const uint8_t pat[] = { 0x66,0xFF,0x75 };  // push word [ebp+var]
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    if (p[4] == 0x66 && p[5] == 0x9D)
                        mem_write(p + 4, NOP2, 2);
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            // -------------------------------------------------------------------
            // Fix 7: NOP PEB.BeingDebugged XOR in XLiveInitialize (xlive_5000)
            //
            // During XLiveInitialize, xlive reads PEB.BeingDebugged and XORs
            // xlive's central state variable with 0x2B7 if set, corrupting the
            // state machine that drives _XNetStartup and all networking calls.
            {
                static const uint8_t pat[] = {
                    0x64,0xA1,0x18,0x00,0x00,0x00,  // mov eax, fs:[18h]
                    0x8B,0x40,0x30,                  // mov eax, [eax+30h]
                    0x0F,0xB6,0x40,0x02,             // movzx eax, [eax+2]
                    0x8B,0x4D,0xD4,                  // mov ecx, [ebp+hModule]
                    0x89,0x01,                        // mov [ecx], eax
                    0x39,0x55,0xD8,                  // cmp [ebp+var_28], edx
                    0x74,0x0A                         // jz +Ah
                };
                patch_all(pat, sizeof(pat), sizeof(pat), NOP10, 10);
            }
        }

        // -------------------------------------------------------------------------
        // PEB.BeingDebugged cleaner

        void clear_being_debugged()
        {
            // Clear only PEB.BeingDebugged (byte at PEB+2). Single-byte writes are
            // atomic on x86. Do NOT touch NtGlobalFlag or heap flags — those are
            // accessed concurrently by HeapAlloc/HeapFree and would cause corruption.
            const DWORD teb = __readfsdword(0x18);
            reinterpret_cast<BYTE*>(*reinterpret_cast<ULONG_PTR*>(teb + 0x30))[2] = 0;
        }

        void hook_ntqip()
        {
            const auto ntdll = GetModuleHandleA("ntdll.dll");
            if (!ntdll) return;
            const auto fn = GetProcAddress(ntdll, "NtQueryInformationProcess");
            if (!fn) return;
            ntqip_hook.create(fn, NtQueryInformationProcess_hook);
        }

        DWORD WINAPI cleaner_thread(LPVOID)
        {
            while (true) { clear_being_debugged(); Sleep(1); }
            return 0;
        }

    } // anonymous namespace

    // -------------------------------------------------------------------------

    void apply_early()
    {
        if (!GetModuleHandleA("xlive.dll"))
        {
            MessageBoxA(nullptr, "xlive.dll not loaded — patches skipped", "xlive", MB_OK);
            return;
        }
        ensure_fake_pool();
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
#ifdef DEBUG
    REGISTER_COMPONENT(xlive::component) //only register in debug
#endif