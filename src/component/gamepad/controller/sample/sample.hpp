#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/sample/axis.hpp>
#include <component/gamepad/controller/sample/button.hpp>
#include <component/gamepad/controller/sample/trigger.hpp>
#include <component/gamepad/controller/sample/touch.hpp>
#include <component/gamepad/controller/sample/motion.hpp>

#include <component/gamepad/controller/device/capability.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Battery state at the time of a sample.
      //
      struct battery_state
      {
        enum class status : uint8_t
        {
          unknown,
          discharging,
          charging,
          full,
        };

        status             state {status::unknown};
        optional<uint8_t>  percent;  // 0..100 when the device reports a level.
      };

      const char*
      to_string (battery_state::status) noexcept;

      // Minimally-processed, device-specific reading.
      //
      // A raw sample is what the driver decoded straight from the report, before
      // any device-neutral interpretation. It exists so calibration can measure a
      // real device and so diagnostics can show exactly what arrived; the game path
      // never reads it. Its fields carry device-native units and bit layouts, so it
      // is only meaningful together with the device's family.
      //
      struct raw_sample
      {
        array<stick_raw, stick_count>       sticks {};
        array<uint16_t, trigger_count>      triggers {};
        uint32_t                            buttons {0};  // Device-native button bits.
        optional<motion_sample>             motion;
        optional<touchpad>                  touch;
        optional<battery_state>             battery;
      };

      // Device-neutral, game-facing input state.
      //
      // The canonical sample is loss-aware: an absent optional means "this device
      // does not report this," which is a different fact from a present-but-zero
      // value. A DualSense fills touch and motion; an XInput pad leaves them empty;
      // neither is coerced into the other's shape. caps records what the producing
      // device can actually do, so a consumer can present or withhold a feature
      // without consulting the driver.
      //
      struct canonical_sample
      {
        button_set                          buttons {};
        array<stick_sample, stick_count>    sticks {};
        array<trigger_sample, trigger_count> triggers {};
        optional<touchpad>                  touch;
        optional<motion_sample>             motion;
        optional<battery_state>             battery;
        capabilities                        caps {};
      };
    }
  }
}
