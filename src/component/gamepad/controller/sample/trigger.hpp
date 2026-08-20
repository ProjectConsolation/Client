#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Which analog trigger a reading belongs to.
      //
      enum class trigger_side : uint8_t
      {
        left,
        right,
      };

      inline constexpr size_t trigger_count {2};

      const char*
      to_string (trigger_side) noexcept;

      // One analog trigger reading.
      //
      // raw preserves the device units (XInput and the PlayStation families both
      // report 0..255) for calibration and diagnostics; normalized is the
      // device-neutral [0, 1] value the game path consumes after the trigger
      // deadzone is applied. The digital "pressed" decision is a mapping concern
      // and is not baked in here.
      //
      struct trigger_sample
      {
        uint16_t raw {0};
        float    normalized {0.0f};
      };
    }
  }
}
