#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace menu_slideshow
{
    constexpr std::uint64_t interval_ms = 8000, fade_ms = 1500;
    inline float background_progress(std::uint64_t elapsed)
    {
        // Two-minute round trip, with zero velocity at either end.
        const float phase = static_cast<float>(elapsed % 120000) / 60000.0f;
        const float t = phase <= 1 ? phase : 2 - phase;
        return t * t * (3 - 2 * t);
    }
    struct frame { std::size_t previous, current; float previous_progress, progress, alpha; };
    inline frame sample(std::uint64_t elapsed, std::size_t count)
    {
        if (!count) return {};
        const auto step = elapsed / interval_ms;
        const auto phase = elapsed % interval_ms;
        const float t = std::min(1.0f, static_cast<float>(phase) / fade_ms);
        return {static_cast<std::size_t>((step + count - 1) % count),
            static_cast<std::size_t>(step % count),
            std::min(1.0f, static_cast<float>(interval_ms + phase) / (interval_ms + fade_ms)),
            static_cast<float>(phase) / (interval_ms + fade_ms), t * t * (3.0f - 2.0f * t)};
    }
    struct crop { float s0, t0, s1, t1; };
    inline crop cover(float image_aspect, float window_aspect, std::size_t /*slide*/, float progress)
    {
        if (!(image_aspect > 0) || !(window_aspect > 0)) return {0, 0, 1, 1};
        const float width = std::min(1.0f, window_aspect / image_aspect) / 1.12f;
        const float height = std::min(1.0f, image_aspect / window_aspect) / 1.12f;
        progress = std::clamp(progress, 0.0f, 1.0f);
        // Moving the sampling window left makes the picture move right.
        // Animate UVs only: the destination quad never leaves its card.
        const float x = 1 - progress, y = .5f;
        return {(1 - width) * x, (1 - height) * y,
            (1 - width) * x + width, (1 - height) * y + height};
    }
}
