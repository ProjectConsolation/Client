#pragma once

namespace exception
{
	bool write_minidump_file(LPEXCEPTION_POINTERS exceptioninfo, const char* filename);
}
