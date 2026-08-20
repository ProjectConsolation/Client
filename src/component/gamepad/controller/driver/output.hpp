#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/sample/trigger.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Dual eccentric-rotating-mass rumble.
        //
        // Both intensities are normalized to [0, 1]; the driver scales them to the
        // device's own range. On the DualSense the same request drives the voice-coil
        // actuators, so rumble is the common output every supported family honors.
        //
        struct rumble_request
        {
          float low_frequency {0.0f};   // Large, low-frequency motor (typically left).
          float high_frequency {0.0f};  // Small, high-frequency motor (typically right).
        };

        // RGB light bar colour (DualShock 4 / DualSense).
        //
        struct light_bar_request
        {
          uint8_t red {0};
          uint8_t green {0};
          uint8_t blue {0};
        };

        // Player-indicator LEDs (DualSense).
        //
        // The low five bits select which of the five LEDs are lit.
        //
        struct player_led_request
        {
          uint8_t mask {0};
        };

        // High-level adaptive-trigger effect (DualSense / DualSense Edge).
        //
        // The request is device-neutral: it says what effect to produce, not how to
        // encode it. The driver translates it into the device's raw trigger-effect
        // bytes, which is exactly the encoding that must never be guessed, so it lives
        // behind the driver rather than in this vocabulary.
        //
        enum class trigger_effect : uint8_t
        {
          off,        // Release the trigger; no resistance.
          feedback,   // Constant resistance from a position.
          weapon,     // Resistance across a range with a release "break".
          vibration,  // Oscillating resistance across a range.
        };

        struct adaptive_trigger_request
        {
          trigger_side   side {trigger_side::left};
          trigger_effect effect {trigger_effect::off};

          uint8_t start_position {0};  // 0..9 region start, effect-dependent.
          uint8_t end_position {0};    // 0..9 region end, effect-dependent.
          uint8_t strength {0};        // 0..8 resistance strength.
        };

        // A single output the runtime asks a driver to apply.
        //
        // A variant rather than a bag of optional fields, so a driver matches on the
        // one thing it is being asked to do and rejects what it cannot honor. There
        // is deliberately no "raw report" alternative: the runtime never hands a
        // driver bytes to emit blindly.
        //
        using output_request = variant<rumble_request,
                                        light_bar_request,
                                        player_led_request,
                                        adaptive_trigger_request>;
      }
    }
  }
}
