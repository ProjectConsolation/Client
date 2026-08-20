#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>
#include <component/gamepad/controller/aim/types.hpp>
#include <component/gamepad/controller/aim/deadzone.hpp>
#include <component/gamepad/controller/aim/curve.hpp>
#include <component/gamepad/controller/aim/graph.hpp>
#include <component/gamepad/controller/aim/integrator.hpp>
#include <component/gamepad/controller/sample/axis.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // Turn-rate parameters for one aiming context (hip fire or ADS, and separately
        // for pitch and yaw). The rates are the maxima the stick can command; the
        // deadzone and curve shape how deflection maps onto them.
        //
        struct aim_profile
        {
          deg_per_s      yaw_rate {0.0f};
          deg_per_s      pitch_rate {0.0f};
          deadzone_params deadzone;
          response_curve curve;
        };

        // Everything the aim processor needs about one frame.
        //
        // look is the right-stick vector in canonical space (up and right positive),
        // already calibrated. ads_lerp blends the hip and ADS profiles (0 hip, 1 ADS).
        // fov_scale and sensitivity scale the turn rate. slowdown_yaw/pitch are the
        // target-slowdown scales in [0, 1] (1 is no slowdown); compute them with
        // slowdown_scale. The optional caps are the engine's hard turn-rate limits.
        //
        struct aim_frame_input
        {
          stick_vector       look;
          float              ads_lerp {0.0f};
          float              fov_scale {1.0f};
          float              sensitivity {1.0f};
          float              slowdown_yaw {1.0f};
          float              slowdown_pitch {1.0f};
          bool               invert_pitch {false};
          optional<deg_per_s> yaw_max;
          optional<deg_per_s> pitch_max;
          seconds            dt {0.0f};
        };

        // Angle deltas to apply to the view this frame.
        //
        // yaw_delta is positive turning right; pitch_delta is positive looking up
        // before inversion. The engine layer maps these onto its own view convention.
        //
        struct aim_frame_output
        {
          degrees yaw_delta {0.0f};
          degrees pitch_delta {0.0f};
        };

        // Stateful aim processor.
        //
        // It owns the pitch and yaw integrators, so acceleration and deceleration
        // persist across frames. For a given configuration and a given sequence of
        // per-frame inputs the output is fully deterministic. An optional graph, when
        // present, scales deflection in place of the profile curve; the graph is owned
        // by the configuration and must outlive the processor.
        //
        class aim_processor
        {
        public:
          struct config
          {
            aim_profile             hip;
            aim_profile             ads;
            turn_integrator::limits accel;
            const aim_graph*        graph {nullptr};
          };

          explicit
          aim_processor (config);

          aim_frame_output
          process (const aim_frame_input&) noexcept;

          void
          reset () noexcept;

        private:
          config          cfg_;
          turn_integrator yaw_;
          turn_integrator pitch_;
        };

        // Compute a target-slowdown scale.
        //
        // When a target is present the scale is the hip/ADS blend; otherwise it is 1
        // (no slowdown). This is the pure part of slowdown; the engine decides target
        // presence from its aim-assist target region tests.
        //
        float
        slowdown_scale (bool target_present,
                        float hip_scale,
                        float ads_scale,
                        float ads_lerp) noexcept;

        // Attenuate the lesser of the two view axes by how far the greater dominates.
        //
        // A stick held near a cardinal direction should turn cleanly along it rather
        // than drifting off; a stick held on a diagonal should do both at full rate.
        // The subordinate axis is scaled by one minus the difference of the magnitudes,
        // so an exactly diagonal deflection is untouched and a purely horizontal one
        // suppresses pitch entirely. The vector is a normalized deflection, not an
        // angle, and the transform preserves each component's sign.
        //
        // This is what the gpad aim_scale_view_axis dvar enables.
        //
        stick_vector
        scale_dominant_axis (stick_vector look) noexcept;

        // Velocity-relative lock-on target information, in world space.
        //
        // The view axes must point along the positive output directions so a
        // target drifting that way produces a follow (not an away) correction:
        // view_yaw_axis points RIGHT (+yaw), view_pitch_axis points UP (+pitch).
        // Note the engine's viewAxis[1] is the LEFT vector, so the caller must
        // negate it for view_yaw_axis.
        //
        struct lock_on_target
        {
          world_vector target_velocity;
          world_vector player_velocity;
          world_vector view_pitch_axis;
          world_vector view_yaw_axis;
          float        distance {0.0f};
        };

        struct lock_on_params
        {
          float yaw_strength {0.0f};
          float pitch_strength {0.0f};
        };

        // Additional view deltas that track a locked-on target.
        //
        // The model converts the target's velocity relative to the player, projected
        // onto the view axes, into an angular rate over the arc at the target's
        // distance, then integrates it over the step. A non-positive distance yields
        // no adjustment.
        //
        aim_frame_output
        lock_on (const lock_on_target&, const lock_on_params&, seconds dt) noexcept;
      }
    }
  }
}
