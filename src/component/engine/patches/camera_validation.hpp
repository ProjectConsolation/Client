#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace camera_validation
{
    // Modify only the viewmodel projection's X/Y terms. Preserve aspect ratio,
    // depth mapping, and native ADS interpolation; zero selects stock behavior.
    inline bool viewmodel_projection(std::span<float, 16> matrix, float fov, float ads)
    {
        if (!std::isfinite(fov) || fov < 1.0f || fov > 160.0f || !std::isfinite(ads)
            || !std::isfinite(matrix[0]) || !std::isfinite(matrix[5]) || matrix[5] <= 0.0f) return false;
        const float desired_y = 1.0f / (0.75f * std::tan(fov * 0.00872664626f));
        const float ratio = 1.0f + (desired_y / matrix[5] - 1.0f) * (1.0f - std::clamp(ads, 0.0f, 1.0f));
        if (!std::isfinite(ratio) || ratio <= 0.0f) return false;
        matrix[0] *= ratio;
        matrix[5] *= ratio;
        return true;
    }
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
