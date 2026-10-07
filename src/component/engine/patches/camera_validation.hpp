#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace camera_validation
{
    inline bool finite(std::span<const float> values)
    {
        return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
    }

    // Publish the complete orientation, never a mixture of corrupt camera and
    // predicted angles. Valid native camera output is left untouched.
    inline bool restore_angles(std::span<float, 3> camera, std::span<const float, 3> predicted)
    {
        if (finite(camera) || !finite(predicted)) return false;
        std::copy(predicted.begin(), predicted.end(), camera.begin());
        return true;
    }

    inline bool restore_relative_axis(std::span<float, 9> axis)
    {
        if (finite(axis)) return false;
        std::fill(axis.begin(), axis.end(), 0.0f);
        axis[0] = axis[4] = axis[8] = 1.0f;
        return true;
    }
}
