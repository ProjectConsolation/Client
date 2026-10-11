#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#pragma comment(lib, "bcrypt.lib")

// Local emulation identity, not a Discord/GFWL account or authentication token.
// Keep the old profile directory so adopting an independent XUID cannot erase
// existing balance, settings or loadouts. Records live outside distributable
// game storage, so sending a game folder to a friend does not copy the XUID.
namespace offline_identity
{
    struct record
    {
        std::uint64_t magic = 0x3144494C5343ull; // CSLID1
        std::uint64_t xuid = 0;
        std::uint64_t profile = 0;
    };
    static_assert(sizeof(record) == 24);

    inline record resolve(const std::filesystem::path& directory,
        unsigned int instance, std::uint64_t legacy_profile)
    {
        std::filesystem::create_directories(directory);
        const auto path = directory / (std::to_wstring(instance) + L".bin");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto error = GetLastError();
        const bool created = file != INVALID_HANDLE_VALUE;
        if (!created && error == ERROR_FILE_EXISTS)
            file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot access offline identity; refusing to replace it");
        struct close_file { HANDLE file; ~close_file() { CloseHandle(file); } } close{file};
        record value;
        DWORD bytes = 0;
        if (created)
        {
            std::uint64_t random = 0;
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random),
                sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("Cannot generate offline identity");
            value.xuid = 0xE000000000000000ull | (random & 0x0000FFFFFFFFFFFFull);
            value.profile = legacy_profile;
            if (!WriteFile(file, &value, sizeof(value), &bytes, nullptr)
                || bytes != sizeof(value) || !FlushFileBuffers(file))
                throw std::runtime_error("Cannot save offline identity");
        }
        else
        {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) || size.QuadPart != sizeof(value)
                || !ReadFile(file, &value, sizeof(value), &bytes, nullptr)
                || bytes != sizeof(value) || value.magic != record{}.magic
                || (value.xuid & 0xFFFF000000000000ull) != 0xE000000000000000ull
                || (value.profile & 0xFFFF000000000000ull) != 0xE000000000000000ull)
                throw std::runtime_error("Invalid offline identity; restore its backup rather than resetting it");
        }
        return value;
    }
}
