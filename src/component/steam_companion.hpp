#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace steam_companion
{
    // Independently declared interoperability layouts from IW4x Companion's
    // manual, protocol v1/build 2 (20261013201752.e59688e36fde), section 5.
    // https://github.com/iw4x/companion -- no library code is linked into d3d9.
    struct record
    {
        std::uint32_t magic, version, size, process_id, app_id;
        char extra_info[64];
    };
    struct status_record
    {
        std::uint32_t magic, version, size, state, build, hook;
    };
    static_assert(sizeof(record) == 84 && offsetof(record, extra_info) == 20);
    static_assert(sizeof(status_record) == 24 && offsetof(status_record, state) == 12);

    class client
    {
    public:
        client() = default;
        ~client() { shutdown(); }
        client(const client&) = delete;
        client& operator=(const client&) = delete;
        void initialize(std::uint32_t app);
        void update(const std::string& detail);
        void print_status() const;
        void retry();
        void shutdown();
    private:
        void attach();
        bool read_status(status_record& result) const;
        HANDLE mapping_ = nullptr, changed_ = nullptr, steam_ = nullptr, helper_ = nullptr;
        record* record_ = nullptr;
        DWORD steam_pid_ = 0;
        bool attempted_ = false;
        std::string title_;
    };
}
