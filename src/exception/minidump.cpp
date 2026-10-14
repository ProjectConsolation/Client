#include <std_include.hpp>
#include "minidump.hpp"

#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <gsl/gsl>

namespace exception
{
	namespace
	{
		constexpr MINIDUMP_TYPE get_minidump_type()
		{
			const auto type = MiniDumpIgnoreInaccessibleMemory //
				| MiniDumpWithDataSegs //
				| MiniDumpWithHandleData //
				| MiniDumpScanMemory //
				| MiniDumpWithProcessThreadData //
				| MiniDumpWithFullMemoryInfo //
				| MiniDumpWithThreadInfo //
				| MiniDumpWithUnloadedModules;

			return static_cast<MINIDUMP_TYPE>(type);
		}

	}

	bool write_minidump_file(const LPEXCEPTION_POINTERS exceptioninfo, const char* filename)
	{
		// Write directly to disk: wtaf.dmp caught bad_alloc when the x86 exit
		// reporter tried to append an entire large dump to a string.
		const auto file_handle = CreateFileA(filename, GENERIC_WRITE | GENERIC_READ,
			FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file_handle == INVALID_HANDLE_VALUE) return false;

		const auto _ = gsl::finally([file_handle]()
		{
			CloseHandle(file_handle);
		});

		MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), exceptioninfo, FALSE};
		const bool written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file_handle,
			get_minidump_type(), exceptioninfo ? &info : nullptr, nullptr, nullptr) != FALSE;
		return written && FlushFileBuffers(file_handle) != FALSE;
	}
}
