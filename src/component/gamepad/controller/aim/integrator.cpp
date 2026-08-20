#include <std_include.hpp>

#include <component/gamepad/controller/aim/integrator.hpp>

#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        degrees
        turn_integrator::
        advance (deg_per_s target, const limits& lim, seconds dt) noexcept
        {
          const float dtc (dt.count ());

          // A non-positive step advances nothing; leave the rate as it is so a zero
          // or negative frame time cannot snap it to the target.
          //
          if (dtc <= 0.0f)
            return degrees {0.0f};

          const float prev (current_.value);
          const float tgt (target.value);
          float cur (prev);

          if (cur < tgt)
          {
            // Speeding up: rise toward the target no faster than the acceleration
            // limit; a zero limit rises instantly.
            //
            const float step (lim.accel.value <= 0.0f ? (tgt - cur)
                                                      : lim.accel.value * dtc);
            cur = std::min (cur + step, tgt);
          }
          else if (cur > tgt)
          {
            // Slowing down: fall toward the target no faster than the deceleration
            // limit; a zero limit falls instantly.
            //
            const float step (lim.decel.value <= 0.0f ? (cur - tgt)
                                                      : lim.decel.value * dtc);
            cur = std::max (cur - step, tgt);
          }

          current_ = deg_per_s {cur};

          // Angle covered this step is the area under the rate ramp: the average of
          // the start and end rate times the step.
          //
          return degrees {0.5f * (prev + cur) * dtc};
        }
      }
    }
  }
}
