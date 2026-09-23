#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <utils/io.hpp>
#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>

#include <exception/minidump.hpp>
#include "game/game.hpp"

namespace exception
{
    namespace
    {
        volatile LONG handling_exception = 0;
        volatile LONG writing_exit_dump = 0;
        utils::hook::detour set_unhandled_exception_filter_hook;
        decltype(&ExitProcess) exit_process_original = nullptr;
        decltype(&TerminateProcess) terminate_process_original = nullptr;

#ifdef DEBUG
        constexpr DWORD stack_buffer_overrun = 0xC0000409;
        constexpr auto report_gs_failure_terminate_return = 0x10138651;
        constexpr auto gs_exception_record_address = 0x10576DF8;
        constexpr auto gs_context_address = 0x10576E50;
#endif

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

        std::string write_minidump(const LPEXCEPTION_POINTERS exceptioninfo,
            const char* kind = "crash")
        {
            const auto timestamp = utils::string::get_timestamp();
            const std::string crash_name = utils::string::va("minidumps/consolation-%s-%s.dmp",
                kind, timestamp.data());
            const auto dump = create_minidump(exceptioninfo);
            if (utils::io::write_file(crash_name, dump, false))
            {
                return crash_name;
            }

            char temp_path[MAX_PATH]{};
            const auto temp_path_length = GetTempPathA(ARRAYSIZE(temp_path), temp_path);
            if (temp_path_length > 0 && temp_path_length < ARRAYSIZE(temp_path))
            {
                const std::string fallback_name = utils::string::va(
                    "%sProject-Consolation-%s-%s.dmp", temp_path, kind, timestamp.data());
                if (utils::io::write_file(fallback_name, dump, false))
                {
                    return fallback_name;
                }
            }

            return {};
        }

#ifdef DEBUG
        void write_exit_minidump(const DWORD code, void* const address)
        {
            if (InterlockedExchange(&writing_exit_dump, 1) != 0 || handling_exception != 0)
            {
                return;
            }

            CONTEXT context{};
            RtlCaptureContext(&context);

            EXCEPTION_RECORD record{};
            record.ExceptionCode = code;
            record.ExceptionAddress = address;

            EXCEPTION_POINTERS pointers{&record, &context};
            const auto dump_name = write_minidump(&pointers, "exit");
            const auto diagnostic = utils::string::va(
                "[exception] explicit process exit code=0x%08X caller=%p dump=%s\n",
                code, address, dump_name.empty() ? "<write failed>" : dump_name.c_str());
            OutputDebugStringA(diagnostic);
            std::printf("%s", diagnostic);
            std::fflush(stdout);
        }

        bool write_gs_failure_minidump(const DWORD code, void* const caller)
        {
            if (code != stack_buffer_overrun ||
                caller != reinterpret_cast<void*>(game::game_offset(report_gs_failure_terminate_return)))
            {
                return false;
            }

            if (InterlockedExchange(&writing_exit_dump, 1) != 0 || handling_exception != 0)
            {
                return true;
            }

            auto* const record = reinterpret_cast<EXCEPTION_RECORD*>(
                game::game_offset(gs_exception_record_address));
            auto* const context = reinterpret_cast<CONTEXT*>(game::game_offset(gs_context_address));
            EXCEPTION_POINTERS pointers{record, context};
            const auto dump_name = write_minidump(&pointers, "gs-failure");
            const auto diagnostic = utils::string::va(
                "[exception] /GS failure code=0x%08X address=%p eip=%p dump=%s\n",
                record->ExceptionCode, record->ExceptionAddress,
                reinterpret_cast<void*>(context->Eip),
                dump_name.empty() ? "<write failed>" : dump_name.c_str());
            OutputDebugStringA(diagnostic);
            std::printf("%s", diagnostic);
            std::fflush(stdout);
            return true;
        }

        DECLSPEC_NORETURN void WINAPI exit_process_stub(const UINT code)
        {
            write_exit_minidump(code, _ReturnAddress());
            exit_process_original(code);
        }

        BOOL WINAPI terminate_process_stub(const HANDLE process, const UINT code)
        {
            if (GetProcessId(process) == GetCurrentProcessId())
            {
                const auto caller = _ReturnAddress();
                if (!write_gs_failure_minidump(code, caller))
                {
                    write_exit_minidump(code, caller);
                }
            }

            return terminate_process_original(process, code);
        }
#endif

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
            const std::string dump_status = dump_name.empty()
                ? "\r\nThe minidump could not be written."
                : "\r\nA minidump was written to:\r\n" + dump_name;
            const auto message = generate_crash_info(exceptioninfo) + dump_status +
                "\r\n\r\nMake sure your graphics card drivers and operating system are up to date.";

            display_error_dialog(message, "Project: Consolation ERROR");
            TerminateProcess(GetCurrentProcess(), code);

            return EXCEPTION_CONTINUE_SEARCH;
        }

        LPTOP_LEVEL_EXCEPTION_FILTER WINAPI set_unhandled_exception_filter_stub(
            const LPTOP_LEVEL_EXCEPTION_FILTER filter)
        {
            // The QoS PC 1.1 CRT clears the process filter before forwarding
            // abort, Watson, and /GS failures to UnhandledExceptionFilter.
            // Preserve Consolation's dump handler for those fatal paths.
            if (!filter)
            {
                return exception_filter;
            }

            const auto original = reinterpret_cast<decltype(&SetUnhandledExceptionFilter)>(
                set_unhandled_exception_filter_hook.get_original());
            return original(filter);
        }

    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            SetUnhandledExceptionFilter(exception_filter);
            set_unhandled_exception_filter_hook.create(
                reinterpret_cast<void*>(SetUnhandledExceptionFilter),
                set_unhandled_exception_filter_stub);

#ifdef DEBUG
            // QoS PC 1.1 imports used by engine-controlled shutdown paths.
            const auto exit_process_iat = reinterpret_cast<decltype(&ExitProcess)*>(
                game::game_offset(0x10476110));
            const auto terminate_process_iat = reinterpret_cast<decltype(&TerminateProcess)*>(
                game::game_offset(0x104761D8));
            exit_process_original = *exit_process_iat;
            terminate_process_original = *terminate_process_iat;
            utils::hook::set(exit_process_iat, exit_process_stub);
            utils::hook::set(terminate_process_iat, terminate_process_stub);
#endif

            constexpr auto message = "[exception] unhandled exception filter installed\n";
            OutputDebugStringA(message);
            std::printf("%s", message);
            std::fflush(stdout);
        }

        void pre_destroy() override
        {
#ifdef DEBUG
            if (exit_process_original)
            {
                utils::hook::set(game::game_offset(0x10476110), exit_process_original);
            }
            if (terminate_process_original)
            {
                utils::hook::set(game::game_offset(0x104761D8), terminate_process_original);
            }
#endif
        }
    };
}

REGISTER_COMPONENT(exception::component)
