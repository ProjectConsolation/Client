#include <std_include.hpp>

#include <component/gamepad/controller/calibration/normalize.hpp>

#include <cmath>
#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace calibration
      {
        void
        apply (const profile& p,
               const raw_sample& raw,
               canonical_sample& canonical) noexcept
        {
          // Sticks: centre and range correction on the driver's normalized vector.
          //
          for (size_t i (0); i < stick_count; ++i)
          {
            const stick_calibration& c (p.sticks[i]);
            stick_sample& s (canonical.sticks[i]);

            float cx ((s.normalized.x - c.center_x) / c.range_x);
            float cy ((s.normalized.y - c.center_y) / c.range_y);

            cx = std::clamp (cx, -1.0f, 1.0f);
            cy = std::clamp (cy, -1.0f, 1.0f);

            float m (std::sqrt (cx * cx + cy * cy));
            if (m > 1.0f)
            {
              cx /= m;
              cy /= m;
              m = 1.0f;
            }

            // Residual drift below the threshold is snapped to rest.
            //
            if (m <= c.drift_threshold)
            {
              cx = 0.0f;
              cy = 0.0f;
            }

            s.calibrated = {cx, cy};
          }

          // Triggers: rescale the normalized value to the measured travel, replacing
          // the driver's straight normalization.
          //
          for (size_t i (0); i < trigger_count; ++i)
          {
            const trigger_calibration& tc (p.triggers[i]);
            trigger_sample& t (canonical.triggers[i]);

            const float denom (tc.max - tc.min);
            const float v (denom > 0.0f ? (t.normalized - tc.min) / denom
                                        : t.normalized);
            t.normalized = std::clamp (v, 0.0f, 1.0f);
          }

          // Motion: debias and scale the raw counts into physical units. Only produced
          // when the device actually reported motion.
          //
          if (raw.motion)
          {
            const motion_calibration& mc (p.motion);
            motion_sample out (*raw.motion);

            const sensor_vec3 g (out.gyro.angular_velocity);
            out.gyro.angular_velocity =
              {(g.x - mc.gyro_bias.x) * mc.gyro_scale,
               (g.y - mc.gyro_bias.y) * mc.gyro_scale,
               (g.z - mc.gyro_bias.z) * mc.gyro_scale};

            const sensor_vec3 a (out.accel.acceleration);
            out.accel.acceleration =
              {(a.x - mc.accel_bias.x) * mc.accel_scale,
               (a.y - mc.accel_bias.y) * mc.accel_scale,
               (a.z - mc.accel_bias.z) * mc.accel_scale};

            canonical.motion = out;
          }
        }
      }
    }
  }
}
