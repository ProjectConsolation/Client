#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // One-pole low-pass (exponential smoothing) filter with frame-time-
        // independent response.
        //
        // The smoothing factor is derived from the time constant and the actual step,
        // so the same physical smoothing results at any frame rate rather than the
        // frame-rate-dependent smoothing a fixed per-frame blend would give. A larger
        // time constant smooths more; a zero time constant passes the input through.
        //
        // The first sample after a reset adopts the target exactly, so the filter does
        // not ramp up from zero when it starts.
        //
        class low_pass
        {
        public:
          explicit
          low_pass (seconds time_constant) noexcept;

          // Advance the filter toward target over dt and return the smoothed value.
          //
          float
          apply (float target, seconds dt) noexcept;

          // Prime the filter at value; the next apply blends from here.
          //
          void
          reset (float value) noexcept;

          // Unprime the filter; the next apply adopts its target exactly.
          //
          void
          reset () noexcept;

          float
          value () const noexcept {return state_;}

        private:
          seconds tau_;
          float   state_ {0.0f};
          bool    primed_ {false};
        };
      }
    }
  }
}
