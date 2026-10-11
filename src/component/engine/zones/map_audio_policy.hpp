#pragma once
#include <string>
#include <string_view>

namespace fastfiles::map_audio
{
    // Content-driven companion convention. No map-name allowlist. Main map
    // and companion share native ownership, so map transitions release both.
    inline std::string companion_name(std::string_view name)
    {
        if (!name.starts_with("mp_") || name.size() <= 3 || name.size() > 57
            || name.ends_with("_load") || name.ends_with("_audio")
            || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string_view::npos)
            return {};
        return std::string(name) + "_audio";
    }
}
