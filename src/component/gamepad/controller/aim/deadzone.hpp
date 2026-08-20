#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/aim/types.hpp>
#include <component/gamepad/controller/sample/axis.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // Radial deadzone parameters, all as fractions of full deflection.
        //
        //   inner  Magnitude at or below which the output is zero. Removes centre
        //          noise and resting drift.
        //   outer  Distance from full deflection at which the output saturates to 1,
        //          so a stick that cannot physically reach the corner still produces
        //          full output.
        //   anti   Minimum non-zero output once past the inner zone, for compatibility
        //          with engines that expect immediate response off centre. Zero
        //          disables it.
        //
        // Valid iff each lies in [0, 1) and inner < 1 - outer, so that the mapping
        // range is non-empty. Validation is explicit because these come from user
        // configuration.
        //
        struct deadzone_params
        {
          magnitude inner {0.0f};
          magnitude outer {0.0f};
          magnitude anti {0.0f};
        };

        // Validate parameters. On failure returns false and sets why to a specific
        // reason; the caller reports it and keeps the previous parameters.
        //
        bool
        validate (const deadzone_params&, string& why) noexcept;

        // Apply a radial deadzone to a stick vector.
        //
        // The magnitude is remapped: zero at or below inner, ramping linearly to one
        // at (1 - outer) and clamped there beyond, with the anti-deadzone floor
        // applied once non-zero. The direction is preserved exactly. The mapping is
        // deterministic and hits its boundaries exactly. The parameters are assumed
        // valid (validate () has accepted them); the invariant is asserted.
        //
        stick_vector
        apply (const deadzone_params&, stick_vector) noexcept;
      }
    }
  }
}
