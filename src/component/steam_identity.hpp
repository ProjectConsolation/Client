#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace steam_identity
{
    inline bool reserved_clan(std::string_view text)
    {
        std::string normalized;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            auto ch = static_cast<unsigned char>(text[i]);
            if (ch == '^' && i + 1 < text.size()) { ++i; continue; }
            if (ch <= 32 || ch == 127) continue;
            if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            normalized += static_cast<char>(ch);
        }
        return normalized == "CSL";
    }

    inline bool clan_allowed(std::uint64_t signed_in_steam_id, std::string_view tag)
    {
        return !reserved_clan(tag) || signed_in_steam_id == 76561198390318660ull;
    }

    inline bool filter_clan_userinfo(std::string& info, const bool authorized)
    {
        if (authorized) return false;
        std::string result;
        bool changed = false;
        std::size_t pos = !info.empty() && info.front() == '\\' ? 1 : 0;
        while (pos < info.size())
        {
            const auto key_end = info.find('\\', pos);
            if (key_end == std::string::npos)
            {
                // Preserve a malformed tail for native validation, but do not
                // let it undo filtering of an earlier valid reserved tag.
                if (changed) info = result + "\\" + info.substr(pos);
                return changed;
            }
            const auto value_end = info.find('\\', key_end + 1);
            const auto key = info.substr(pos, key_end - pos);
            auto value = info.substr(key_end + 1, value_end == std::string::npos ? std::string::npos : value_end - key_end - 1);
            std::string lower = key;
            for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            if (lower == "clanabbrev" && reserved_clan(value)) { value.clear(); changed = true; }
            result += "\\" + key + "\\" + value;
            if (value_end == std::string::npos) break;
            pos = value_end + 1;
        }
        if (changed) info = std::move(result);
        return changed;
    }

    // QoS GamerProfile_LogInProfile (1024B670) caches 15 bytes + NUL.
    // Preserve UTF-8 boundaries, and reject userinfo/control/color syntax.
    inline std::string display_name(std::string_view source)
    {
        std::string result;
        for (std::size_t i = 0; i < source.size();)
        {
            const auto lead = static_cast<unsigned char>(source[i]);
            const std::size_t size = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2
                : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
            if (!size) { ++i; continue; }
            if (i + size > source.size() || result.size() + size > 15) break;
            bool valid = true;
            for (std::size_t j = 1; j < size; ++j)
                valid &= (static_cast<unsigned char>(source[i + j]) & 0xC0) == 0x80;
            if (size >= 3)
            {
                const auto next = static_cast<unsigned char>(source[i + 1]);
                valid &= !(lead == 0xE0 && next < 0xA0) && !(lead == 0xED && next >= 0xA0)
                    && !(lead == 0xF0 && next < 0x90) && !(lead == 0xF4 && next >= 0x90);
            }
            if (valid && (size != 1 || (lead >= 32 && lead != 127 && lead != '^' && lead != '\\' && lead != '"')))
                result.append(source.substr(i, size));
            i += size;
        }
        return result.empty() ? "Player" : result;
    }

    class policy
    {
    public:
        bool initialized() const { return !current_.empty(); }
        const std::string& update(const std::string& desired, const bool match)
        {
            if (!match || current_.empty()) current_ = display_name(desired);
            return current_;
        }
    private:
        std::string current_;
    };
}
