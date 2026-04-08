#pragma once

namespace exception
{
	std::string create_minidump(LPEXCEPTION_POINTERS exceptioninfo);
	std::string create_minidump();
}
