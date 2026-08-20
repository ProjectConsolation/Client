#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/sample/axis.hpp>
#include <component/gamepad/controller/sample/button.hpp>
#include <component/gamepad/controller/mapping/key.hpp>

#include <variant>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        // A stick pushed far enough in a direction to count as a discrete press.
        //
        enum class stick_direction : uint8_t
        {
          up,
          down,
          left,
          right,
        };

        // A stick deflection treated as a directional press Ã¢â‚¬â€ the engine's analog
        // pad ("apad") input, used for menu navigation. which records the stick that
        // produced it for diagnostics; the engine key depends only on the direction,
        // since both sticks drive the same four apad keys.
        //
        struct apad_input
        {
          stick           which {stick::left};
          stick_direction direction {stick_direction::up};
        };

        // A discrete physical input a player actuates.
        //
        // Either a canonical physical button or an apad deflection. This is what the
        // hardware produces, kept distinct from the engine key it maps to and from
        // the action a key is bound to.
        //
        using physical_input = variant<button, apad_input>;

        // Map a physical button to its engine key.
        //
        // Returns nullopt for buttons the engine has no key for Ã¢â‚¬â€ the PS/guide button,
        // touchpad click, microphone mute, and the DualSense Edge rear buttons. Those
        // are decoded and available to the subsystem but are not part of the engine's
        // controller key set.
        //
        optional<engine_key>
        to_engine_key (button) noexcept;

        // Map an apad deflection to its engine key.
        //
        engine_key
        to_engine_key (const apad_input&) noexcept;

        // Map any physical input to its engine key, if it has one.
        //
        optional<engine_key>
        to_engine_key (const physical_input&) noexcept;

        // Whether a stick axis reads as deflected far enough to be a press.
        //
        // value is one component of a shaped (deadzoned) stick vector, so it is a
        // normalized scalar in [-1, 1]. positive selects which end of the axis is
        // being tested. was_down is the decision this function returned for the same
        // axis and end on the previous sample.
        //
        // Hysteresis moves the threshold against the direction of travel Ã¢â‚¬â€ outward by
        // hysteresis while the axis is up, inward by hysteresis while it is down Ã¢â‚¬â€ so
        // an axis resting near the threshold latches instead of chattering between
        // pressed and released on consecutive frames.
        //
        bool
        axis_deflected (float value,
                        bool positive,
                        bool was_down,
                        float pressed,
                        float hysteresis) noexcept;
      }
    }
  }
}
