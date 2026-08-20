#include <std_include.hpp>

#include <component/gamepad/controller/aim/filter.hpp>

#include <cmath>
#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        low_pass::
        low_pass (seconds time_constant) noexcept
          : tau_ (time_constant)
        {
        }

        float
        low_pass::
        apply (float target, seconds dt) noexcept
        {
          // A non-positive time constant means no smoothing; the first sample after a
          // reset adopts the target so the filter does not ramp from zero.
          //
          if (tau_.count () <= 0.0f)
            return target;

          if (!primed_)
          {
            state_ = target;
            primed_ = true;
            return state_;
          }

          // The blend factor for a one-pole filter over a step dt with time constant
          // tau is 1 - exp(-dt/tau). Deriving it from the real step is what makes the
          // response frame-time independent.
          //
          const float alpha (
            std::clamp (1.0f - std::exp (-dt.count () / tau_.count ()), 0.0f, 1.0f));

          state_ += alpha * (target - state_);
          return state_;
        }

        void
        low_pass::
        reset (float value) noexcept
        {
          state_ = value;
          primed_ = true;
        }

        void
        low_pass::
        reset () noexcept
        {
          primed_ = false;
        }
      }
    }
  }
}
