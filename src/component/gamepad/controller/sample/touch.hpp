#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // One touchpad contact.
      //
      // Coordinates are in the device's own touch resolution (for the DualShock 4
      // and DualSense this is 1920 x 943), not a normalized or screen space; a
      // consumer that wants screen space converts explicitly. id is the hardware's
      // contact tracking identifier, which increments as new touches begin and lets
      // a consumer follow one finger across frames. When active is false the
      // remaining fields are stale and must not be read.
      //
      struct touch_point
      {
        bool     active {false};
        uint8_t  id {0};
        uint16_t x {0};
        uint16_t y {0};
      };

      // Touchpad state for one sample.
      //
      // The DualShock 4 and DualSense report up to two simultaneous contacts. A
      // device without a touchpad publishes no touchpad state at all (the canonical
      // sample's optional is left empty) rather than an all-inactive one, so that
      // "no touchpad" and "touchpad not touched" stay distinguishable.
      //
      struct touchpad
      {
        static constexpr size_t max_points {2};

        array<touch_point, max_points> points {};
      };
    }
  }
}
