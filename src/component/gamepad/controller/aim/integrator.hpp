#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>
#include <component/gamepad/controller/aim/types.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // Frame-time-independent turn-rate integrator with controlled acceleration
        // and deceleration.
        //
        // Each step advances the current turn rate toward a target rate, changing it
        // no faster than the acceleration limit when speeding up or the deceleration
        // limit when slowing down, and returns the angle covered during the step. The
        // covered angle uses the average of the rate at the start and end of the step
        // (trapezoidal integration of the piecewise-linear rate ramp), so subdividing
        // a step does not change the total angle or the end rate beyond integration
        // error Ã¢â‚¬â€ that is what makes the turn frame-time independent.
        //
        // A zero acceleration or deceleration limit means that side is instantaneous:
        // the rate snaps to the target with no ramp.
        //
        class turn_integrator
        {
        public:
          struct limits
          {
            deg_per_s2 accel {0.0f};
            deg_per_s2 decel {0.0f};
          };

          // Advance one step toward target and return the angle covered.
          //
          degrees
          advance (deg_per_s target, const limits&, seconds dt) noexcept;

          deg_per_s
          current () const noexcept {return current_;}

          void
          reset () noexcept {current_ = deg_per_s {0.0f};}

        private:
          deg_per_s current_ {0.0f};
        };
      }
    }
  }
}
