#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace steam_friends
{
    // Architecture reference: KisakBlack live_friends_pc.cpp's SteamID-backed
    // sorted friends and ui_friends.cpp's selection/paging. No BO1 ABI imported.
    struct entry { std::uint64_t id{}, lobby{}; std::string name, status; bool online{}; };
    inline std::string label(const char* value)
    {
        std::string result;
        if (!value) return result;
        for (std::size_t i = 0; value[i] && i < 128; ++i)
        {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c < 32 || c == 127 || c == '^' || c == '|' || c == '"' || c == ';') continue;
            result += static_cast<char>(c);
        }
        // Never cut through a UTF-8 continuation sequence at the display bound.
        if (result.size() > 48)
        {
            std::size_t end = 48;
            while (end && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
            result.resize(end);
        }
        return result.empty() ? "Player" : result;
    }
    class snapshot
    {
    public:
        static constexpr std::size_t page_size = 8;
        std::vector<entry> entries;
        std::uint64_t selected = 0;
        std::size_t page = 0;
        std::size_t first_visible = 0;
        void scroll_to(const std::size_t first)
        {
            first_visible = std::min(first, entries.size() > page_size ? entries.size() - page_size : 0);
            page = first_visible / page_size;
        }
        void update(std::vector<entry> incoming)
        {
            if (incoming.size() > 256) incoming.resize(256);
            std::sort(incoming.begin(), incoming.end(), [](const entry& a, const entry& b) {
                if (a.online != b.online) return a.online > b.online;
                if (a.name != b.name) return a.name < b.name;
                return a.id < b.id;
            });
            entries = std::move(incoming);
            if (!get(selected)) selected = 0;
            page = std::min(page, pages() - 1);
            scroll_to(first_visible);
        }
        std::size_t pages() const { return std::max(std::size_t{1}, (entries.size() + page_size - 1) / page_size); }
        const entry* get(std::uint64_t id) const
        {
            const auto it = std::find_if(entries.begin(), entries.end(), [id](const entry& e) { return id && e.id == id; });
            return it == entries.end() ? nullptr : &*it;
        }
    };
}
