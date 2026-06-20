#include <std_include.hpp>
#include <utils/hook.hpp>
#include "loader/component_loader.hpp"
#include "xlive.hpp"

namespace xlive
{
	namespace
	{
		constexpr auto status_success = 0x00000000L;
		constexpr auto status_port_not_set = 0xC0000353L;
		constexpr auto process_debug_port = 7u;
		constexpr auto process_debug_object_handle = 0x1Eu;
		constexpr auto process_debug_flags = 0x1Fu;
		constexpr auto thread_hide_from_debugger = 0x11u;
		constexpr auto system_kernel_debugger_information = 0x23u;
		constexpr auto peverifyhash_rva = 0x000F36B3u;

		utils::hook::detour nt_query_information_process_hook;
		utils::hook::detour nt_set_information_thread_hook;
		utils::hook::detour nt_set_information_process_hook;
		utils::hook::detour nt_query_system_information_hook;
		utils::hook::detour nt_get_context_thread_hook;
		utils::hook::detour nt_set_context_thread_hook;
		utils::hook::detour nt_set_debug_filter_state_hook;
		utils::hook::detour nt_yield_execution_hook;

#ifdef DEBUG
		void dbg(const char* fmt, ...)
		{
			char buf[512]{};
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			OutputDebugStringA("[xlive] ");
			OutputDebugStringA(buf);
			OutputDebugStringA("\n");
		}
#else
		void dbg(const char*, ...) {}
#endif

		using nt_query_information_process_t = LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG, PULONG);
		using nt_set_information_thread_t = LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG);
		using nt_set_information_process_t = LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG);
		using nt_query_system_information_t = LONG(__stdcall*)(UINT, PVOID, ULONG, PULONG);
		using nt_get_context_thread_t = LONG(__stdcall*)(HANDLE, PCONTEXT);
		using nt_set_context_thread_t = LONG(__stdcall*)(HANDLE, const CONTEXT*);
		using nt_set_debug_filter_state_t = LONG(__stdcall*)(ULONG, ULONG, BOOLEAN);
		using nt_yield_execution_t = LONG(__stdcall*)();

		BOOL WINAPI is_debugger_present_stub()
		{
			return FALSE;
		}

		BOOL WINAPI check_remote_debugger_present_stub(HANDLE, PBOOL present)
		{
			if (present)
			{
				*present = FALSE;
			}

			return TRUE;
		}

		void WINAPI output_debug_string_a_stub(LPCSTR)
		{
		}

		void WINAPI output_debug_string_w_stub(LPCWSTR)
		{
		}

		LONG __stdcall nt_query_information_process_stub(HANDLE process, UINT info_class, PVOID info, ULONG info_len, PULONG ret_len)
		{
			if (info_class == process_debug_port)
			{
				if (info && info_len >= sizeof(ULONG_PTR))
				{
					*static_cast<ULONG_PTR*>(info) = 0;
				}

				if (ret_len)
				{
					*ret_len = sizeof(ULONG_PTR);
				}

				return status_success;
			}

			if (info_class == process_debug_object_handle)
			{
				if (info && info_len >= sizeof(HANDLE))
				{
					*static_cast<HANDLE*>(info) = nullptr;
				}

				if (ret_len)
				{
					*ret_len = sizeof(HANDLE);
				}

				return status_port_not_set;
			}

			if (info_class == process_debug_flags)
			{
				if (info && info_len >= sizeof(ULONG))
				{
					*static_cast<ULONG*>(info) = 1;
				}

				if (ret_len)
				{
					*ret_len = sizeof(ULONG);
				}

				return status_success;
			}

			return nt_query_information_process_hook.invoke<LONG>(process, info_class, info, info_len, ret_len);
		}

		LONG __stdcall nt_set_information_thread_stub(HANDLE thread, UINT info_class, PVOID info, ULONG info_len)
		{
			if (info_class == thread_hide_from_debugger)
			{
				return status_success;
			}

			return nt_set_information_thread_hook.invoke<LONG>(thread, info_class, info, info_len);
		}

		LONG __stdcall nt_set_information_process_stub(HANDLE process, UINT info_class, PVOID info, ULONG info_len)
		{
			if (info_class == process_debug_flags)
			{
				return status_success;
			}

			return nt_set_information_process_hook.invoke<LONG>(process, info_class, info, info_len);
		}

		LONG __stdcall nt_query_system_information_stub(UINT info_class, PVOID info, ULONG info_len, PULONG ret_len)
		{
			const auto result = nt_query_system_information_hook.invoke<LONG>(info_class, info, info_len, ret_len);

			if (result == status_success && info_class == system_kernel_debugger_information && info && info_len >= 2)
			{
				auto* const bytes = static_cast<BYTE*>(info);
				bytes[0] = FALSE; // KernelDebuggerEnabled
				bytes[1] = TRUE;  // KernelDebuggerNotPresent
			}

			return result;
		}

		LONG __stdcall nt_get_context_thread_stub(HANDLE thread, PCONTEXT context)
		{
			const auto result = nt_get_context_thread_hook.invoke<LONG>(thread, context);

			if (result == status_success && context && (context->ContextFlags & CONTEXT_DEBUG_REGISTERS) != 0)
			{
				context->Dr0 = 0;
				context->Dr1 = 0;
				context->Dr2 = 0;
				context->Dr3 = 0;
				context->Dr6 = 0;
				context->Dr7 = 0;
			}

			return result;
		}

		LONG __stdcall nt_set_context_thread_stub(HANDLE thread, const CONTEXT* context)
		{
			if (context && (context->ContextFlags & CONTEXT_DEBUG_REGISTERS) != 0)
			{
				auto sanitized = *context;
				sanitized.Dr0 = 0;
				sanitized.Dr1 = 0;
				sanitized.Dr2 = 0;
				sanitized.Dr3 = 0;
				sanitized.Dr6 = 0;
				sanitized.Dr7 = 0;
				return nt_set_context_thread_hook.invoke<LONG>(thread, &sanitized);
			}

			return nt_set_context_thread_hook.invoke<LONG>(thread, context);
		}

		LONG __stdcall nt_set_debug_filter_state_stub(ULONG, ULONG, BOOLEAN)
		{
			return status_success;
		}

		LONG __stdcall nt_yield_execution_stub()
		{
			return nt_yield_execution_hook.invoke<LONG>();
		}

		bool mem_write(void* dst, const void* src, const size_t len)
		{
			DWORD old = 0;
			if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old))
			{
				return false;
			}

			memcpy(dst, src, len);
			FlushInstructionCache(GetCurrentProcess(), dst, len);
			VirtualProtect(dst, len, old, &old);
			return true;
		}

		uint8_t* scan(uint8_t* base, const size_t size, const uint8_t* pat, const size_t pat_len)
		{
			if (!pat_len || size < pat_len)
			{
				return nullptr;
			}

			for (size_t i = 0; i <= size - pat_len; ++i)
			{
				if (!memcmp(base + i, pat, pat_len))
				{
					return base + i;
				}
			}

			return nullptr;
		}

		bool get_module_range(const char* name, uint8_t*& base, size_t& image_size)
		{
			base = reinterpret_cast<uint8_t*>(GetModuleHandleA(name));
			if (!base)
			{
				return false;
			}

			const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			{
				return false;
			}

			const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
			{
				return false;
			}

			image_size = nt->OptionalHeader.SizeOfImage;
			return true;
		}

		void clear_peb_debug_flags()
		{
			const auto teb = __readfsdword(0x18);
			const auto peb = *reinterpret_cast<uint8_t**>(teb + 0x30);
			if (!peb)
			{
				return;
			}

			peb[2] = 0; // BeingDebugged
			*reinterpret_cast<DWORD*>(peb + 0x68) = 0; // NtGlobalFlag on x86
		}

		void patch_xlive_imports(uint8_t* base, const size_t image_size)
		{
			__try
			{
				const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
				const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				const auto import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;

				if (!import_rva || import_rva >= image_size)
				{
					return;
				}

				auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + import_rva);
				for (; desc->Name && desc->Name < image_size; ++desc)
				{
					const auto* const dll = reinterpret_cast<const char*>(base + desc->Name);
					if (_stricmp(dll, "kernel32.dll") != 0 && _stricmp(dll, "kernel32") != 0)
					{
						continue;
					}

					if (!desc->OriginalFirstThunk || !desc->FirstThunk)
					{
						continue;
					}

					auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
					auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

					for (; original_thunk->u1.AddressOfData; ++original_thunk, ++thunk)
					{
						if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)
							|| original_thunk->u1.AddressOfData >= image_size)
						{
							continue;
						}

						const auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original_thunk->u1.AddressOfData);
						const auto* const name = reinterpret_cast<const char*>(import->Name);
						void* replacement = nullptr;

						if (strcmp(name, "IsDebuggerPresent") == 0)
						{
							replacement = reinterpret_cast<void*>(&is_debugger_present_stub);
						}
						else if (strcmp(name, "CheckRemoteDebuggerPresent") == 0)
						{
							replacement = reinterpret_cast<void*>(&check_remote_debugger_present_stub);
						}
						else if (strcmp(name, "OutputDebugStringA") == 0)
						{
							replacement = reinterpret_cast<void*>(&output_debug_string_a_stub);
						}
						else if (strcmp(name, "OutputDebugStringW") == 0)
						{
							replacement = reinterpret_cast<void*>(&output_debug_string_w_stub);
						}

						if (replacement)
						{
							mem_write(&thunk->u1.Function, &replacement, sizeof(replacement));
							dbg("patched xlive import %s", name);
						}
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		void patch_peverifyhash(uint8_t* base, const size_t image_size)
		{
			static const uint8_t prologue[] = {
				0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20,
				0x53, 0x56, 0x57, 0x8D, 0x45, 0xE0, 0x33, 0xF6,
				0x50, 0xFF, 0x75, 0x0C, 0x8B, 0xF9
			};
			static const uint8_t ret_success[] = { 0x31, 0xC0, 0xC2, 0x0C, 0x00 };

			auto* target = scan(base, image_size, prologue, sizeof(prologue));
			if (!target && peverifyhash_rva + sizeof(ret_success) <= image_size)
			{
				target = base + peverifyhash_rva;
			}

			if (!target)
			{
				dbg("PEVerifyHash patch skipped: signature missing");
				return;
			}

			if (!memcmp(target, ret_success, sizeof(ret_success)))
			{
				dbg("PEVerifyHash patch already applied");
				return;
			}

			if (memcmp(target, prologue, sizeof(ret_success)) != 0)
			{
				dbg("PEVerifyHash patch skipped: unexpected bytes at +0x%X", static_cast<unsigned>(target - base));
				return;
			}

			mem_write(target, ret_success, sizeof(ret_success));
			dbg("PEVerifyHash patch applied at +0x%X", static_cast<unsigned>(target - base));
		}

		void patch_xlive()
		{
			uint8_t* base = nullptr;
			size_t image_size = 0;

			if (!get_module_range("xlive.dll", base, image_size))
			{
				return;
			}

			patch_peverifyhash(base, image_size);
			patch_xlive_imports(base, image_size);
		}

		void create_nt_hook(const char* name, void* stub, utils::hook::detour& hook)
		{
			const auto ntdll = GetModuleHandleA("ntdll.dll");
			if (!ntdll)
			{
				return;
			}

			const auto target = GetProcAddress(ntdll, name);
			if (!target)
			{
				return;
			}

			hook.create(target, stub);
			dbg("hooked %s", name);
		}

		void hook_ntdll()
		{
			create_nt_hook("NtQueryInformationProcess", nt_query_information_process_stub, nt_query_information_process_hook);
			create_nt_hook("NtSetInformationThread", nt_set_information_thread_stub, nt_set_information_thread_hook);
			create_nt_hook("NtSetInformationProcess", nt_set_information_process_stub, nt_set_information_process_hook);
			create_nt_hook("NtQuerySystemInformation", nt_query_system_information_stub, nt_query_system_information_hook);
			create_nt_hook("NtGetContextThread", nt_get_context_thread_stub, nt_get_context_thread_hook);
			create_nt_hook("NtSetContextThread", nt_set_context_thread_stub, nt_set_context_thread_hook);
			create_nt_hook("NtSetDebugFilterState", nt_set_debug_filter_state_stub, nt_set_debug_filter_state_hook);
			create_nt_hook("NtYieldExecution", nt_yield_execution_stub, nt_yield_execution_hook);
		}

		DWORD WINAPI peb_cleaner_thread(LPVOID)
		{
			while (true)
			{
				clear_peb_debug_flags();
				Sleep(16);
			}
		}
	}

	void apply_early()
	{
		dbg("apply_early: begin");
		clear_peb_debug_flags();
		hook_ntdll();
		patch_xlive();
		CloseHandle(CreateThread(nullptr, 0, peb_cleaner_thread, nullptr, 0, nullptr));
		dbg("apply_early: done");
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			clear_peb_debug_flags();
			patch_xlive();
		}
	};
}

#ifdef DEBUG
//REGISTER_COMPONENT(xlive::component)
#endif
