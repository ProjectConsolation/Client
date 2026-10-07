#include "../src/component/engine/patches/camera_validation.hpp"
#include <array>
#include <cassert>
#include <limits>

int main()
{
    const std::array<float, 3> predicted{75.58044f, 153.41858f, 0.0f};
    auto camera = predicted;
    assert(!camera_validation::restore_angles(camera, predicted));
    camera[0] = std::numeric_limits<float>::quiet_NaN();
    camera[1] = std::numeric_limits<float>::infinity();
    assert(camera_validation::restore_angles(camera, predicted));
    assert(camera == predicted);
    auto bad_predicted = predicted;
    bad_predicted[2] = std::numeric_limits<float>::quiet_NaN();
    camera[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!camera_validation::restore_angles(camera, bad_predicted));
    assert(std::isnan(camera[0]));
    std::array<float, 9> axis{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto identity = axis;
    assert(!camera_validation::restore_relative_axis(axis));
    assert(axis == identity);
    axis[5] = std::numeric_limits<float>::quiet_NaN();
    assert(camera_validation::restore_relative_axis(axis));
    assert(axis == identity);
}
