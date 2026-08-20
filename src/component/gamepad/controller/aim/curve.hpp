#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // A closed-form response-curve family.
        //
        // Every family maps an input fraction in [0, 1] to an output in [0, 1], is
        // monotonic non-decreasing, and fixes the endpoints (0 to 0, 1 to 1). A curve
        // therefore changes the feel of stick response without changing its range,
        // which is what lets hip and ADS share a range while differing in shape.
        //
        enum class curve_kind : uint8_t
        {
          linear,  // Output equals input.
          power,   // Output is input raised to exponent; exponent 1 is linear.
        };

        const char*
        to_string (curve_kind) noexcept;

        struct response_curve
        {
          curve_kind kind {curve_kind::linear};
          float      exponent {1.0f};
        };

        // Validate a curve. A power curve requires a finite exponent greater than
        // zero, so it stays monotonic and endpoint-preserving. On failure returns
        // false and sets why.
        //
        bool
        validate (const response_curve&, string& why) noexcept;

        // Evaluate the curve at t. t is clamped to [0, 1]; the result is in [0, 1].
        // Deterministic. Assumes the curve has been validated.
        //
        float
        evaluate (const response_curve&, float t) noexcept;
      }
    }
  }
}
