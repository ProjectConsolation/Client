#include <std_include.hpp>
#include "steam_companion.hpp"
#include "engine/console/console.hpp"
#include <TlHelp32.h>
#include <atomic>

namespace steam_companion
{
    namespace
    {
        DWORD steam_pid()
        {
            SYSTEM_INFO system{};
            GetNativeSystemInfo(&system);
            if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) return 0;
            DWORD session = 0;
            if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return 0;
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return 0;
            PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
            DWORD found = 0;
            if (Process32FirstW(snapshot, &entry)) do
            {
                if (_wcsicmp(entry.szExeFile, L"steam.exe")) continue;
                DWORD target_session = 0;
                if (!ProcessIdToSessionId(entry.th32ProcessID, &target_session) || target_session != session) continue;
                const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                BOOL wow64 = TRUE;
                const bool valid = process && IsWow64Process(process, &wow64) && !wow64;
                if (process) CloseHandle(process);
                if (!valid) continue;
                if (found) { found = 0; break; } // ambiguous; never attach to an arbitrary Steam
                found = entry.th32ProcessID;
            } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
            return found;
        }
    }

    void client::initialize(const std::uint32_t app)
    {
        if (record_ || !app || app >= (1u << 24)) return;
        const auto name = L"Local\\Companion.SteamPresence." + std::to_wstring(GetCurrentProcessId());
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(record), name.c_str());
        if (!mapping_) return;
        record_ = static_cast<record*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(record)));
        if (!record_) { CloseHandle(mapping_); mapping_ = nullptr; return; }
        std::memset(record_, 0, sizeof(record));
        record_->version = 1; record_->size = sizeof(record);
        record_->process_id = GetCurrentProcessId(); record_->app_id = app;
        update("");
    }

    bool client::read_status(status_record& result) const
    {
        if (!steam_pid_) return false;
        const auto name = L"Local\\Companion.Steam." + std::to_wstring(steam_pid_);
        const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!mapping) return false;
        const auto* view = static_cast<const status_record*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(status_record)));
        bool valid = false;
        if (view)
        {
            // Build 2 publishes state last. Load only, never interlocked-write
            // a status mapping owned by Companion (this view is read-only).
            result.state = std::atomic_ref<std::uint32_t>(const_cast<status_record*>(view)->state).load(std::memory_order_acquire);
            result.magic = view->magic; result.version = view->version; result.size = view->size;
            result.build = view->build; result.hook = view->hook;
            valid = result.magic == 0x53345749 && result.version == 1 && result.size == sizeof(status_record) && result.state <= 3;
            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
        return valid;
    }

    void client::attach()
    {
		if (helper_ || attempted_ || !steam_pid_) return;
        attempted_ = true;
        status_record status{};
        if (read_status(status)) return; // reuse resident module; never unload it
        wchar_t image[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, image, MAX_PATH)) return;
        const auto dll = std::filesystem::path(image).parent_path() / L"companion64.dll";
        std::error_code error;
        if (!std::filesystem::is_regular_file(dll, error))
        { console::warn("Companion: companion64.dll missing beside the game; Steam matchmaking remains available.\n"); return; }
        // Do not LoadLibrary a 64-bit module into this x86 game. The official
        // attach entry validates the target and performs its own bounded load.
        wchar_t windows[MAX_PATH]{}; BOOL wow64 = FALSE;
        if (!GetWindowsDirectoryW(windows, MAX_PATH) || !IsWow64Process(GetCurrentProcess(), &wow64)) return;
        const auto rundll = std::filesystem::path(windows) / (wow64 ? L"Sysnative" : L"System32") / L"rundll32.exe";
        // Upstream protocol names the base entry companion_attach. rundll32
        // resolves companion_attachW first and supplies its UTF-16 arguments.
        std::wstring command = L"\"" + rundll.wstring() + L"\" \"" + dll.wstring() + L"\",companion_attach " + std::to_wstring(steam_pid_);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(rundll.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dll.parent_path().c_str(), &startup, &process))
        { console::warn("Companion: attach helper could not start (Windows error %lu).\n", GetLastError()); return; }
        CloseHandle(process.hThread); helper_ = process.hProcess;
    }

    bool client::prepare()
    {
        if (helper_ && WaitForSingleObject(helper_, 0) == WAIT_OBJECT_0)
        {
            DWORD code = 0; GetExitCodeProcess(helper_, &code);
            console::info("Companion: attach helper result=%lu (0=active, 1=unsupported, 2=failed, 7=timeout).\n", code);
            CloseHandle(helper_); helper_ = nullptr;
        }
        if (steam_ && WaitForSingleObject(steam_, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(steam_); steam_ = nullptr;
            if (changed_) CloseHandle(changed_);
            changed_ = nullptr; steam_pid_ = 0; attempted_ = false; preparation_started_ = 0;
        }
        if (!steam_)
        {
            steam_pid_ = steam_pid();
            if (steam_pid_) steam_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, steam_pid_);
            if (!steam_) steam_pid_ = 0;
            if (steam_pid_)
            {
                preparation_started_ = GetTickCount64();
                const auto event = L"Local\\Companion.SteamPresence.Changed." + std::to_wstring(steam_pid_);
                changed_ = CreateEventW(nullptr, FALSE, FALSE, event.c_str());
                if (changed_) SetEvent(changed_);
            }
        }
        attach();
        status_record status{};
        const bool valid = read_status(status);
        // Missing DLL, no Steam, unsupported hook, or failed launch must not
        // prevent SDK initialization. Only defer while attachment is pending.
        const bool pending = helper_ || (valid && status.state == 0);
        return !pending || GetTickCount64() - preparation_started_ >= 20000;
    }

    void client::update(const std::string& detail)
    {
        prepare();
        if (!record_) return;
        std::string title = "Project Consolation";
        if (!detail.empty()) title += " | " + detail;
        // Status details supplied by the adapter are ASCII, never peer text.
        if (title.size() > 63) title.resize(63);
        if (title != title_)
        {
            std::atomic_ref<std::uint32_t>(record_->magic).store(0, std::memory_order_release);
            std::memset(record_->extra_info, 0, sizeof(record_->extra_info));
            std::memcpy(record_->extra_info, title.data(), title.size());
            std::atomic_ref<std::uint32_t>(record_->magic).store(0x50345749, std::memory_order_release);
            title_ = std::move(title);
            if (changed_) SetEvent(changed_);
        }
    }

    void client::print_status() const
    {
        status_record status{};
        console::info("Companion: registration=%d steam_pid=%lu title=%s\n", record_ != nullptr, steam_pid_, title_.c_str());
        if (read_status(status)) console::info("Companion: state=%u (0=starting,1=active,2=unsupported,3=failed) build=%u hook=0x%X\n", status.state, status.build, status.hook);
        else console::info("Companion: no compatible module status; registration alone does not prove friends see the title.\n");
    }

    void client::retry()
    {
        if (helper_) { console::info("Companion: attach helper still pending.\n"); return; }
        attempted_ = false; attach(); print_status();
    }

    void client::shutdown()
    {
        if (record_)
        {
            std::atomic_ref<std::uint32_t>(record_->magic).store(0, std::memory_order_release);
            if (changed_) SetEvent(changed_);
            UnmapViewOfFile(record_); record_ = nullptr;
        }
        for (const auto handle : {mapping_, changed_, steam_, helper_}) if (handle) CloseHandle(handle);
        mapping_ = changed_ = steam_ = helper_ = nullptr;
        steam_pid_ = 0; attempted_ = false; preparation_started_ = 0; title_.clear();
        // The helper is bounded by upstream. Never terminate Steam or remotely
        // unload Companion, whose hook must remain resident until Steam exits.
    }
}
