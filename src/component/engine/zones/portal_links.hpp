#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace fastfiles::portal_links
{
    struct link { std::uint32_t cell, portal, destination; };
    struct manifest { std::uint32_t cells{}; std::vector<link> links; };
    inline bool decode(std::span<const char> data, manifest& output)
    {
        if (data.size() < 16 || std::memcmp(data.data(), "CSLP", 4)) return false;
        auto word = [&](std::size_t offset) {
            std::uint32_t value{};
            std::memcpy(&value, data.data() + offset, 4);
            return value;
        };
        const auto cells = word(8), count = word(12);
        if (word(4) != 1 || !cells || cells > 4096 || count > 65536
            || data.size() != 16ull + 12ull * count) return false;
        manifest result{cells, {}};
        result.links.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto offset = 16 + 12 * i;
            link value{word(offset), word(offset + 4), word(offset + 8)};
            if (value.cell >= cells || value.destination >= cells || value.portal >= 65536
                || (!result.links.empty() && (value.cell < result.links.back().cell
                    || (value.cell == result.links.back().cell && value.portal <= result.links.back().portal))))
                return false;
            result.links.push_back(value);
        }
        output = std::move(result);
        return true;
    }
}
