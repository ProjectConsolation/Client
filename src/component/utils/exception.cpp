#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "component/utils/scheduler.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>
#include <atomic>
#include <thread>

#include <exception/minidump.hpp>

namespace exception
{
    namespace
    {
        using namespace std::chrono_literals;

        std::atomic<unsigned long long> last_heartbeat_ms{0};
        std::atomic<bool> watchdog_running{false};

        thread_local struct
        {
            DWORD code = 0;
            PVOID address = nullptr;
        } exception_data;

        void show_mouse_cursor()
        {
            while (ShowCursor(TRUE) < 0);
        }

        void display_error_dialog()
        {
            std::string error_str = utils::string::va("Fatal error (0x%08X) at 0x%p.\n"
                                                      "A minidump has been written.\n\n",
                                                      exception_data.code, exception_data.address);

            error_str += "Make sure to update your graphics card drivers and install operating system updates!";

            utils::thread::suspend_other_threads();
            show_mouse_cursor();
            MessageBoxA(nullptr, error_str.data(), "Project: Consolation ERROR", MB_ICONERROR);
            TerminateProcess(GetCurrentProcess(), exception_data.code);
        }

        void reset_state()
        {
            display_error_dialog();
        }

        size_t get_reset_state_stub()
        {
            static auto* stub = utils::hook::assemble([](utils::hook::assembler& a)
            {
                a.sub(esp, 0x10);
                a.or_(esp, 0x8);
                a.jmp(reset_state);
            });

            return reinterpret_cast<size_t>(stub);
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

        void write_minidump(const LPEXCEPTION_POINTERS exceptioninfo)
        {
            utils::io::create_directory("minidumps");
            const std::string crash_name = utils::string::va("minidumps/consolation-crash-%s.dmp",
                                                             utils::string::get_timestamp().data());
            create_minidump(exceptioninfo);
            utils::io::write_file(crash_name, create_minidump(exceptioninfo), false);
        }

        void write_hang_dump()
        {
            utils::io::create_directory("minidumps");
            const std::string crash_name = utils::string::va("minidumps/consolation-hang-%s.dmp",
                                                             utils::string::get_timestamp().data());
            utils::io::write_file(crash_name, create_minidump(), false);
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

            write_minidump(exceptioninfo);

            exception_data.code = exceptioninfo->ExceptionRecord->ExceptionCode;
            exception_data.address = exceptioninfo->ExceptionRecord->ExceptionAddress;
            exceptioninfo->ContextRecord->Eip = get_reset_state_stub();

            return EXCEPTION_CONTINUE_EXECUTION;
        }

        LPTOP_LEVEL_EXCEPTION_FILTER WINAPI set_unhandled_exception_filter_stub(LPTOP_LEVEL_EXCEPTION_FILTER)
        {
            // Don't register anything here...
            return &exception_filter;
        }

        bool command_line_has(const char* token)
        {
            const auto* const cmd = GetCommandLineA();
            return cmd && token && std::strstr(cmd, token) != nullptr;
        }

        void start_watchdog()
        {
            if (watchdog_running.exchange(true))
            {
                return;
            }

            last_heartbeat_ms = GetTickCount64();

            std::thread([]()
            {
                constexpr auto timeout_ms = 10000ULL;

                while (true)
                {
                    std::this_thread::sleep_for(1s);
                    const auto now = GetTickCount64();
                    const auto last = last_heartbeat_ms.load();
                    if (now - last >= timeout_ms)
                    {
                        write_hang_dump();
                        TerminateProcess(GetCurrentProcess(), 0xDEAD);
                    }
                }
            }).detach();

            scheduler::loop([]()
            {
                last_heartbeat_ms = GetTickCount64();
            }, scheduler::main, 250ms);
        }
    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            if (command_line_has("-no_crashdump"))
            {
                return;
            }

            if (!command_line_has("-no_watchdog"))
            {
                start_watchdog();
            }

            const auto enable_handler = []()
            {
                SetUnhandledExceptionFilter(exception_filter);
                utils::hook::jump(reinterpret_cast<uintptr_t>(&SetUnhandledExceptionFilter), set_unhandled_exception_filter_stub);
            };

            if (command_line_has("-crashdump"))
            {
                enable_handler();
                return;
            }

            // Delay hook to avoid interfering with early boot while still capturing later crashes.
            scheduler::once(enable_handler, scheduler::main, 15s);
        }
    };
}

REGISTER_COMPONENT(exception::component)
