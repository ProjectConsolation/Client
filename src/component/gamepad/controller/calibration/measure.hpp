#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/calibration/profile.hpp>
#include <component/gamepad/controller/sample/axis.hpp>
#include <component/gamepad/controller/sample/motion.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace calibration
      {
        // Derives a stick calibration from observed stick motion.
        //
        // Feed it the driver's normalized stick vectors: while the stick rests to find
        // the centre, and while it is swept around its extent to find the range.
        // finalize () produces a stick_calibration whose range is never degenerate, so
        // its result always passes validation. Allocation-free.
        //
        class stick_measurer
        {
        public:
          void
          observe_rest (stick_vector) noexcept;

          void
          observe_sweep (stick_vector) noexcept;

          stick_calibration
          finalize () const noexcept;

          void
          reset () noexcept;

          size_t
          rest_samples () const noexcept {return rest_n_;}

          size_t
          sweep_samples () const noexcept {return sweep_n_;}

        private:
          double rest_sum_x_ {0.0};
          double rest_sum_y_ {0.0};
          size_t rest_n_ {0};

          float  max_x_ {0.0f};
          float  max_y_ {0.0f};
          size_t sweep_n_ {0};
        };

        // Derives motion-sensor bias from readings taken at rest.
        //
        // Accumulates raw gyroscope and accelerometer counts while the controller is
        // still; finalize () writes the mean as the bias, leaving the scales Ã¢â‚¬â€ which
        // come from the device, not from resting Ã¢â‚¬â€ untouched.
        //
        class motion_bias_measurer
        {
        public:
          void
          observe (const motion_sample&) noexcept;

          void
          finalize (motion_calibration&) const noexcept;

          void
          reset () noexcept;

          size_t
          samples () const noexcept {return n_;}

        private:
          double gx_ {0.0}, gy_ {0.0}, gz_ {0.0};
          double ax_ {0.0}, ay_ {0.0}, az_ {0.0};
          size_t n_ {0};
        };
      }
    }
  }
}
