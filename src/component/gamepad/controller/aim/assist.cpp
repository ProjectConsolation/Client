#include <std_include.hpp>

#include <component/gamepad/controller/aim/assist.hpp>

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
        namespace
        {
          deg_per_s
          lerp_rate (deg_per_s a, deg_per_s b, float t) noexcept
          {
            return {std::lerp (a.value, b.value, t)};
          }

          deadzone_params
          lerp_deadzone (const deadzone_params& a,
                         const deadzone_params& b,
                         float t) noexcept
          {
            return {magnitude {std::lerp (a.inner.value, b.inner.value, t)},
                    magnitude {std::lerp (a.outer.value, b.outer.value, t)},
                    magnitude {std::lerp (a.anti.value, b.anti.value, t)}};
          }
        }

        aim_processor::
        aim_processor (config c)
          : cfg_ (move (c))
        {
        }

        void
        aim_processor::
        reset () noexcept
        {
          yaw_.reset ();
          pitch_.reset ();
        }

        aim_frame_output
        aim_processor::
        process (const aim_frame_input& in) noexcept
        {
          const float t (std::clamp (in.ads_lerp, 0.0f, 1.0f));

          // Deadzone: blend the hip and ADS parameters. A blend of two valid
          // parameter sets stays within the valid ranges, so apply's invariant holds.
          //
          const deadzone_params dz (
            lerp_deadzone (cfg_.hip.deadzone, cfg_.ads.deadzone, t));
          const stick_vector v (apply (dz, in.look));

          const float deflection (
            std::clamp (std::sqrt (v.x * v.x + v.y * v.y), 0.0f, 1.0f));

          // Response scaling on the deflection magnitude: the aim graph when one is
          // configured, otherwise the active profile's curve.
          //
          const float response (
            cfg_.graph != nullptr
              ? cfg_.graph->evaluate (deflection)
              : evaluate (t >= 0.5f ? cfg_.ads.curve : cfg_.hip.curve, deflection));

          const float eff_yaw (v.x * response);    // Right positive.
          const float eff_pitch (v.y * response);  // Up positive.

          // Turn rates: blend hip and ADS, then apply FOV, sensitivity, and the
          // target-slowdown scale. The engine's hard caps, when present, bound them.
          //
          const float gain_yaw (in.fov_scale * in.sensitivity * in.slowdown_yaw);
          const float gain_pitch (in.fov_scale * in.sensitivity * in.slowdown_pitch);

          deg_per_s yaw_rate (
            lerp_rate (cfg_.hip.yaw_rate, cfg_.ads.yaw_rate, t) * gain_yaw);
          deg_per_s pitch_rate (
            lerp_rate (cfg_.hip.pitch_rate, cfg_.ads.pitch_rate, t) * gain_pitch);

          if (in.yaw_max && in.yaw_max->value < yaw_rate.value)
            yaw_rate = *in.yaw_max;
          if (in.pitch_max && in.pitch_max->value < pitch_rate.value)
            pitch_rate = *in.pitch_max;

          // Integrate a non-negative target speed and apply the sign afterward, so a
          // direction reversal flips at the current speed rather than decelerating
          // through zero first. This matches the reference feel.
          //
          const float yaw_sign (eff_yaw >= 0.0f ? 1.0f : -1.0f);
          const float pitch_sign (eff_pitch >= 0.0f ? 1.0f : -1.0f);

          const deg_per_s yaw_target {std::fabs (eff_yaw) * yaw_rate.value};
          const deg_per_s pitch_target {std::fabs (eff_pitch) * pitch_rate.value};

          degrees yaw_delta (yaw_.advance (yaw_target, cfg_.accel, in.dt) * yaw_sign);
          degrees pitch_delta (
            pitch_.advance (pitch_target, cfg_.accel, in.dt) * pitch_sign);

          if (in.invert_pitch)
            pitch_delta = -pitch_delta;

          return {yaw_delta, pitch_delta};
        }

        float
        slowdown_scale (bool target_present,
                        float hip_scale,
                        float ads_scale,
                        float ads_lerp) noexcept
        {
          if (!target_present)
            return 1.0f;

          return std::lerp (hip_scale, ads_scale, std::clamp (ads_lerp, 0.0f, 1.0f));
        }

        stick_vector
        scale_dominant_axis (stick_vector look) noexcept
        {
          const float ax (std::fabs (look.x));
          const float ay (std::fabs (look.y));

          // Only the subordinate axis is attenuated, so the dominant one keeps its full
          // rate. The two magnitudes are in [0, 1], so the factor is too, and an equal
          // deflection leaves both untouched.
          //
          if (ay <= ax)
            look.y *= 1.0f - (ax - ay);
          else
            look.x *= 1.0f - (ay - ax);

          return look;
        }

        aim_frame_output
        lock_on (const lock_on_target& target,
                 const lock_on_params& params,
                 seconds dt) noexcept
        {
          // Without a positive distance there is no arc to integrate over.
          //
          if (target.distance <= 0.0f)
            return {};

          const float arc (target.distance * pi);

          const float pitch_rate (
            (dot (target.target_velocity, target.view_pitch_axis) -
             dot (target.player_velocity, target.view_pitch_axis)) /
            arc * 180.0f * params.pitch_strength);

          const float yaw_rate (
            (dot (target.target_velocity, target.view_yaw_axis) -
             dot (target.player_velocity, target.view_yaw_axis)) /
            arc * 180.0f * params.yaw_strength);

          // Both axes share the "positive follows the target" convention of
          // aim_frame_output: a target rising against the view (positive pitch_rate) is
          // tracked by looking up (positive pitch delta), exactly as a target moving
          // right (positive yaw_rate) is tracked by turning right.
          //
          return {degrees {yaw_rate * dt.count ()},
                  degrees {pitch_rate * dt.count ()}};
        }
      }
    }
  }
}
