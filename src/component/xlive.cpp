/*
 * xlive.cpp — GFWL (Games for Windows Live) anti-debug bypass
 *
 * The retail xlive.dll wraps its public API (XLiveInitialize, XNetStartup,
 * XCloseHandle etc.) inside a protection layer that detects debuggers through
 * multiple independent mechanisms and corrupts internal state on detection.
 * This component patches all known checks in-process so VS can attach freely.
 *
 * All scan patterns target the retail SysWOW64\xlive.dll. The security-disabled
 * SDK build (xlive_debuggable.dll) has none of these protection functions —
 * they are purely injected DRM code absent from the SDK.
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

        // ---------------------------------------------------------------------------
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

        // ---------------------------------------------------------------------------
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

                        if (strcmp(reinterpret_cast<const char*>(ibn->Name), "IsDebuggerPresent") == 0)
                        {
                            const auto stub = reinterpret_cast<void*>(&IsDebuggerPresent_stub);
                            mem_write(&ft->u1.Function, &stub, sizeof(stub));
                            return;
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // ---------------------------------------------------------------------------

        void patch_xlive()
        {
            const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA("xlive.dll"));
            if (!base) return;

            const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const size_t sz = nt->OptionalHeader.SizeOfImage;

            // Scan for a pattern and write replacement bytes at every match.
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
            // Fix 1: IsDebuggerPresent IAT redirect
            //
            // xlive's CRT startup calls IsDebuggerPresent via its own IAT slot.
            // Redirect that slot to a stub that always returns FALSE.
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
            // Fix 2B: bypass INT3 breakpoint scans on the thread-inspect function
            //
            // The protection layer calls XCloseHandle (xlive_5251) as a wrapper
            // for thread handle cleanup after thread-context inspection.
            // xlive checks whether the first byte of this function is 0xCC
            // (an INT3 that VS injects when setting a thread-entry breakpoint)
            // at 14 different call sites. Detection either:
            //   (a) corrupts a function pointer by XOR/SAR/SUB then calls it
            //       → crash at the mangled address
            //   (b) causes the detection setup function to return early, leaving
            //       the low-memory thread stub region unmapped → crash on execute
            //
            // Fix: change every JNZ following the 0xCC comparison to JMP so the
            // "detected" branch is never taken regardless of VS breakpoints.
            {
                // Primary checks — 5-byte form: cmp [reg], 0CCh / jnz +xx
                // JNZ is always at byte offset 3; patch only that byte (75→EB).
                static const uint8_t cc_primary[][5] = {
                    {0x80,0x39,0xCC,0x75,0x1D},  // AntiDbg_CheckThreadExitCode: early exit
                    {0x80,0x39,0xCC,0x75,0x05},  // AntiDbg_CheckBreakpointAndInit: sub eax,221h
                    {0x80,0x3A,0xCC,0x75,0x08},  // AntiDbg_CheckBreakpointAndInit: add eax,2BBh
                    {0x80,0x38,0xCC,0x75,0x10},  // AntiDbg_DetectionChainF: corrupts counter
                    {0x80,0x38,0xCC,0x75,0x04},  // AntiDbg_DetectionChainD: sar [var],1Ah
                    {0x80,0x3A,0xCC,0x75,0x0A},  // AntiDbg_DetectionChainE: or [var],0CCh
                    {0x80,0x38,0xCC,0x75,0x0F},  // AntiDbg_DetectionChainG: sets error 80004005h
                    {0x80,0x38,0xCC,0x75,0x0A},  // AntiDbg_DetectionChainB: imul state by 182h
                    {0x80,0x3A,0xCC,0x75,0x07},  // AntiDbg_DetectionChainA: or [hModule],0CCh
                    {0x80,0x3E,0xCC,0x75,0x0A},  // AntiDbg_DetectionChainC: or [var],0CCh
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
                    // Value-corrupting: crash at XOR'd/shifted address
                    {(const uint8_t*)"\x80\x7D\xC7\xCC\x75\x07\x81\x75\xBC\xDB\x00\x00\x00", 13, 4},
                    // AntiDbg_CheckThreadExitCode: xor [var_44], 0DBh → crash at XOR'd addr
                    {(const uint8_t*)"\x80\x7D\xC3\xCC\x75\x04\xC1\x7D\xBC\x1A",             10, 4},
                    // AntiDbg_DetectionChainD: sar [var_44], 1Ah → near-zero fn ptr
                    {(const uint8_t*)"\x80\xBD\xC3\xFE\xFF\xFF\xCC\x75\x0A\x81\xA5\xBC\xFE\xFF\xFF\x81\x00\x00\x00", 19, 7},
                    // AntiDbg_DetectionHub: and [var_144], 81h → masks detection flags
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x07\x81\x43\x04\xC3\x00\x00\x00", 13, 4},
                    // AntiDbg_Int41Traps: add [ebx+4], 0C3h → writes RET into live code
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x10\x81\x45\xA0\x10\x01\x00\x00", 13, 4},
                    // AntiDbg_DetectionChainA: add [Src], 110h → corrupts string ptr

                    // Early-exit paths: abort detection setup → low-memory stub unmapped
                    {(const uint8_t*)"\x80\xBD\xC3\xFE\xFF\xFF\xCC\x75\x1D",  9, 7},
                    // AntiDbg_DetectionHub: returns early before XCloseHandle stub setup
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x1B",              6, 4},
                    // AntiDbg_Int41Traps: early return path
                    {(const uint8_t*)"\x80\x7D\xAB\xCC\x75\x1C",              6, 4},
                    // AntiDbg_DetectionChainA: early return path
                    {(const uint8_t*)"\x80\xBD\xDB\xFE\xFF\xFF\xCC\x75\x18",  9, 7},
                    // AntiDbg_DetectionChainG: calls AntiDbg_ReportDetection before abort
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
            //
            // Two locations set TryLevel=0 then execute INT 1 inside an SEH frame.
            // The SEH handler catches it and sets a "survived" flag.
            // With a debugger attached VS intercepts INT 1 first; the flag is never
            // set, causing downstream detection.
            // NOP: just the CD 01 — falls through to the "survived" path directly.
            {
                static const uint8_t pat[] = {
                    0x83,0x65,0xFC,0x00,  // and [TryLevel], 0   (enter SEH try block)
                    0xCD,0x01             // int 1               ← NOP these 2 bytes
                };
                patch_all(pat, sizeof(pat), 4, NOP2, 2);
            }

            // Fix 3B: NOP int 3 SEH trap — magic SI/DI sentinel
            //
            // AntiDbg_IntSentinelTrap loads magic constants (SI=4647h, DI=4A4Dh)
            // then executes INT 3 inside an SEH frame. The handler verifies the
            // sentinel values to confirm it was the expected trap source.
            // NOP: just the CC byte — SI/DI stay set, execution falls through.
            {
                static const uint8_t pat[] = {
                    0x66,0xBE,0x47,0x46,  // mov si, 4647h
                    0x66,0xBF,0x4D,0x4A,  // mov di, 4A4Dh
                    0xCC                  // int 3  ← NOP this 1 byte
                };
                patch_all(pat, sizeof(pat), 8, &NOP1, 1);
            }

            // Fix 3C: NOP ud2 SEH trap
            //
            // Sets TryLevel=2 then executes UD2 (undefined instruction) inside an
            // SEH frame. Handler catches EXCEPTION_ILLEGAL_INSTRUCTION and sets
            // a "survived" flag; the state variable [ebx+4] carries the result.
            // NOP: just the 0F 0B — EBX state falls through unchanged.
            {
                static const uint8_t pat[] = {
                    0xC7,0x45,0xFC,0x02,0x00,0x00,0x00,  // mov [TryLevel], 2
                    0x0F,0x0B                              // ud2  ← NOP these 2 bytes
                };
                patch_all(pat, sizeof(pat), 7, NOP2, 2);
            }

            // -------------------------------------------------------------------
            // Fix 4: NOP int 41h SEH traps (5 occurrences)
            //
            // Loads AX=4Fh then executes INT 41h (invalid interrupt vector on x86).
            // The SEH handler stores AX as the "detection passed" result value.
            // NOP: just the CD 41 — AX stays 0x4F, falls through to
            // "mov [var], ax" which is the identical post-handler write.
            {
                static const uint8_t pat[] = {
                    0x66,0xB8,0x4F,0x00,  // mov ax, 4Fh
                    0xCD,0x41             // int 41h  ← NOP these 2 bytes
                };
                patch_all(pat, sizeof(pat), 4, NOP2, 2);
            }

            // -------------------------------------------------------------------
            // Fix 5: NOP divide-by-zero SEH traps (4 occurrences)
            //
            // Executes "xor eax, eax / div eax" — deliberate #DE fault inside
            // an SEH frame. The handler inspects the EXCEPTION_RECORD to verify
            // the expected exception code.
            // NOP: just the F7 F0 — EAX stays 0, falls through to success path.
            {
                static const uint8_t pat[] = {
                    0x33,0xC0,  // xor eax, eax
                    0xF7,0xF0   // div eax  ← NOP these 2 bytes
                };
                patch_all(pat, sizeof(pat), 2, NOP2, 2);
            }

            // -------------------------------------------------------------------
            // Fix 6: NOP POPFW trap-flag traps (4 occurrences)
            //
            // Pushes a FLAGS word with the Trap Flag (TF, bit 8) set onto the stack
            // then executes POPFW to load it into EFLAGS. The CPU immediately raises
            // a single-step exception (#DB) on the next instruction; the SEH handler
            // catches it and records the result.
            // NOP: just the 66 9D (16-bit POPFW) — FLAGS unchanged, no #DB raised.
            {
                static const uint8_t pat[] = { 0x66,0xFF,0x75 };  // push word [ebp+var]
                uint8_t* p = scan(base, sz, pat, sizeof(pat));
                while (p)
                {
                    if (p[4] == 0x66 && p[5] == 0x9D)  // followed by popfw
                        mem_write(p + 4, NOP2, 2);
                    p = scan(p + 1, sz - (p - base) - 1, pat, sizeof(pat));
                }
            }

            // -------------------------------------------------------------------
            // Fix 7: force "clean" path in AntiDbg_CheckThreadExitCode
            //
            // xlive spawns threads whose start address is a Windows handle value
            // (a small integer pointing into a dynamically VirtualAlloc'd stub
            // region). The stub executes, then the parent reads the exit code and
            // runs it through:
            //   and eax, 0D9BB259Ch
            //   cmp eax, 0C153B2h
            //   jb  loc_53D05B        ← "clean" path → returns 0
            //   ; fall-through:  "detected" → invokes AntiDbg_MapLowMemoryStub
            //                    with stale state → crash in obfuscated dispatch
            //
            // With a debugger the thread exit code exceeds the threshold so the
            // JB is never taken. Changing JB (0F 82) to JMP (E9) forces the clean
            // early-exit return (0 = "not detected") unconditionally.
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,       // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00,       // cmp eax, 0C153B2h
                    0x0F,0x82,0xD6,0xFE,0xFF,0xFF   // jb  loc_53D05B
                };
                static const uint8_t FORCE_JMP[6] = { 0xE9,0xD6,0xFE,0xFF,0xFF, 0x90 };
                patch_all(pat, sizeof(pat), 10, FORCE_JMP, 6);
            }

            // -------------------------------------------------------------------
            // Fix 8: force success in AntiDbg_ThreadExitMaskCheck (_XCloseHandle path)
            //
            // Same thread-detection mechanism as Fix 7 but in a separate function
            // (AntiDbg_ThreadExitMaskCheck). This one branches with JNB (jump if
            // result >= threshold → success). With a debugger the threshold
            // comparison fails so JNB is not taken → falls to error return.
            // Change JNB (73) to JMP (EB) — always takes the success path.
            {
                static const uint8_t pat[] = {
                    0x25,0x9C,0x25,0xBB,0xD9,   // and eax, 0D9BB259Ch
                    0x3D,0xB2,0x53,0xC1,0x00,   // cmp eax, 0C153B2h
                    0x73,0x26                    // jnb +26h (success)
                };
                patch_all(pat, sizeof(pat), 10, &JMP_SHORT, 1);
            }

            // -------------------------------------------------------------------
            // Fix 9: NOP PEB.BeingDebugged XOR in XLiveInitialize (xlive_5000)
            //
            // During XLiveInitialize, xlive reads PEB.BeingDebugged (PEB+2),
            // stores it in an internal struct field, then XORs xlive's central
            // state variable with 0x2B7 if the field is nonzero. This corrupts
            // the state machine that drives _XNetStartup (xlive_51) and all
            // subsequent networking calls → XNetStartup returns error.
            //
            // The 10 bytes that follow the pattern perform the XOR; NOP them.
            // When BeingDebugged == 0 (no debugger) the preceding JZ already
            // skips them, so this patch has zero effect on undebugged runs.
            {
                static const uint8_t pat[] = {
                    0x64,0xA1,0x18,0x00,0x00,0x00,  // mov eax, fs:[18h]  (→ TEB)
                    0x8B,0x40,0x30,                  // mov eax, [eax+30h] (→ PEB)
                    0x0F,0xB6,0x40,0x02,             // movzx eax, [eax+2] (BeingDebugged)
                    0x8B,0x4D,0xD4,                  // mov ecx, [ebp+hModule]
                    0x89,0x01,                        // mov [ecx], eax     (store flag)
                    0x39,0x55,0xD8,                  // cmp [ebp+var_28], edx
                    0x74,0x0A                         // jz +Ah             (skip XOR)
                };
                patch_all(pat, sizeof(pat), sizeof(pat), NOP10, 10);
            }
        }

        // ---------------------------------------------------------------------------
        // PEB.BeingDebugged cleaner

        void clear_being_debugged()
        {
            // Clear only PEB.BeingDebugged (byte at PEB+2).
            // Single-byte writes are atomic on x86 so this is safe from any thread.
            // We deliberately do NOT touch NtGlobalFlag or the heap debug flags —
            // those fields are read concurrently by HeapAlloc/HeapFree and writing
            // them from a separate thread causes heap corruption.
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

    // ---------------------------------------------------------------------------

    void apply_early()
    {
        // Called before the first game MessageBox — xlive.dll is already loaded
        // but XLiveInitialize has not been called yet, so all patches land before
        // any protection check executes.
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
            // Re-apply after all components load in case any reload xlive.
            patch_xlive();
            clear_being_debugged();
        }
    };
}

REGISTER_COMPONENT(xlive::component)