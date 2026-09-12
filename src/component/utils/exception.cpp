#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>

#include <exception/minidump.hpp>

namespace exception
{
    namespace
    {
        volatile LONG handling_exception = 0;

        void show_mouse_cursor()
        {
            while (ShowCursor(TRUE) < 0);
        }

        void display_error_dialog(const std::string& message, const char* title)
        {
            utils::thread::suspend_other_threads();
            show_mouse_cursor();
            MessageBoxA(nullptr, message.c_str(), title, MB_ICONERROR | MB_OK);
        }

        std::string generate_crash_info(const LPEXCEPTION_POINTERS exceptioninfo)
        {
            std::string info{};
            const auto line = [&info](const std::string& text)
            {
                info.append(text);
                info.append("\r\n");
            };

            line("Project: Consolation Crash Dump");
            line("");
            line("Timestamp: "s + utils::string::get_timestamp());
            line(utils::string::va("Exception: 0x%08X", exceptioninfo->ExceptionRecord->ExceptionCode));
            line(utils::string::va("Address: 0x%lX", exceptioninfo->ExceptionRecord->ExceptionAddress));

#pragma warning(push)
#pragma warning(disable: 4996)
            OSVERSIONINFOEXA version_info;
            ZeroMemory(&version_info, sizeof(version_info));
            version_info.dwOSVersionInfoSize = sizeof(version_info);
            GetVersionExA(reinterpret_cast<LPOSVERSIONINFOA>(&version_info));
#pragma warning(pop)

            line(utils::string::va("OS Version: %u.%u", version_info.dwMajorVersion, version_info.dwMinorVersion));

            return info;
        }

        std::string write_minidump(const LPEXCEPTION_POINTERS exceptioninfo)
        {
            const std::string crash_name = utils::string::va("minidumps/consolation-crash-%s.dmp",
                                                             utils::string::get_timestamp().data());
            const auto dump = create_minidump(exceptioninfo);
            utils::io::write_file(crash_name, dump, false);
            return crash_name;
        }

        bool is_harmless_error(const LPEXCEPTION_POINTERS exceptioninfo)
        {
            const auto code = exceptioninfo->ExceptionRecord->ExceptionCode;
            return code == STATUS_INTEGER_OVERFLOW || code == STATUS_FLOAT_OVERFLOW || code == STATUS_SINGLE_STEP;
        }

        LONG WINAPI exception_filter(const LPEXCEPTION_POINTERS exceptioninfo)
        {
            if (is_harmless_error(exceptioninfo))
            {
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (InterlockedExchange(&handling_exception, 1) != 0)
            {
                TerminateProcess(GetCurrentProcess(), exceptioninfo->ExceptionRecord->ExceptionCode);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto code = exceptioninfo->ExceptionRecord->ExceptionCode;
            if (code == EXCEPTION_STACK_OVERFLOW)
            {
                display_error_dialog("The game terminated because of a stack overflow.",
                                     "Project: Consolation ERROR");
                TerminateProcess(GetCurrentProcess(), code);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto dump_name = write_minidump(exceptioninfo);
            const auto message = generate_crash_info(exceptioninfo) +
                "\r\nA minidump was written to:\r\n" + dump_name +
                "\r\n\r\nMake sure your graphics card drivers and operating system are up to date.";

            display_error_dialog(message, "Project: Consolation ERROR");
            TerminateProcess(GetCurrentProcess(), code);

            return EXCEPTION_CONTINUE_SEARCH;
        }

    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            SetUnhandledExceptionFilter(exception_filter);
        }
    };
}

REGISTER_COMPONENT(exception::component)
