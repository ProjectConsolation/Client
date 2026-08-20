#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include "component/gamepad/controller/engine/import.hpp"

#include <component/gamepad/controller/context.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        // Typed handles to the controller's engine dvars.
        //
        // The subsystem reads these at the engine boundary and converts them into its
        // own typed values Ã¢â‚¬â€ angles, magnitudes, durations Ã¢â‚¬â€ so a raw dvar float or
        // int never flows deeper than this layer. Each handle is the engine's dvar_t*,
        // resolved once at registration.
        //
        // The surface is kept to what users, calibration, or controlled compatibility
        // actually need; it is not a knob for every internal constant.
        //
        struct dvars
        {
          // Presence and enablement.
          //
          dvar_t* enabled {};        // gpad_enabled
          dvar_t* present {};        // gpad_present   (engine-owned readout)
          dvar_t* in_use {};         // gpad_in_use    (engine-owned readout)
          dvar_t* rumble {};         // gpad_rumble

          // PlayStation light bar. Enabled by a bool; the colour is three channels so a
          // player can retune the default MW2 gold for their own light bar, which reads
          // colour differently from a screen.
          //
          dvar_t* light_bar {};      // gpad_light_bar
          dvar_t* light_bar_r {};    // gpad_light_bar_r
          dvar_t* light_bar_g {};    // gpad_light_bar_g
          dvar_t* light_bar_b {};    // gpad_light_bar_b

          // Stick shaping.
          //
          dvar_t* stick_deadzone_min {};
          dvar_t* stick_deadzone_max {};
          dvar_t* button_deadzone {};
          dvar_t* stick_pressed {};
          dvar_t* stick_pressed_hysteresis {};

          // Named configurations.
          //
          dvar_t* buttons_config {};  // gpad_buttonConfig
          dvar_t* sticks_config {};   // gpad_sticksConfig

          // View and aim.
          //
          dvar_t* invert_pitch {};        // input_invertPitch
          dvar_t* view_sensitivity {};    // input_viewSensitivity
          dvar_t* sensitivity_level {};   // gpad_sensitivity (1-10 console scale)
          dvar_t* aim_assist_enabled {};  // sv_allowAimAssist
          dvar_t* turnrate_pitch {};
          dvar_t* turnrate_pitch_ads {};
          dvar_t* turnrate_yaw {};
          dvar_t* turnrate_yaw_ads {};
          dvar_t* accel_enabled {};       // aim_accel_turnrate_enabled
          dvar_t* accel_rate {};          // aim_accel_turnrate_lerp
          dvar_t* scale_view_axis {};     // aim_scale_view_axis

          // Assist behaviors.
          //
          dvar_t* slowdown_enabled {};
          dvar_t* lockon_enabled {};
          dvar_t* lockon_deflection {};
          dvar_t* lockon_strength {};
          dvar_t* lockon_pitch_strength {};
          dvar_t* aim_assist_range_scale {};
        };

        // Register every controller dvar with the engine.
        //
        // Registration order is fixed and the defaults are the known-working ones, so
        // a fresh profile behaves identically every run. Returns the resolved handles;
        // the count is reported through the context.
        //
        dvars
        register_dvars (const context&);

        // Publish whether any controller is attached.
        //
        // gpad_present is read-only to the player and is how the menus decide whether
        // to offer controller settings at all. Writing it is the engine layer's job,
        // which is why the runtime does not touch the dvar itself.
        //
        void
        publish_present (const dvars&, bool present) noexcept;

        // Read a dvar, falling back to its documented default when the handle is null.
        //
        // The handles are null until the first engine frame registers them, and every
        // reader of a dvar runs before that at least once. Rather than have each caller
        // test the handle, these carry the fallback, which is by construction the same
        // value register_dvars () will install.
        //
        inline bool
        read (dvar_t* d, bool fallback) noexcept
        {
          return d != nullptr ? d->current.enabled : fallback;
        }

        inline float
        read (dvar_t* d, float fallback) noexcept
        {
          return d != nullptr ? d->current.value : fallback;
        }

        inline int
        read (dvar_t* d, int fallback) noexcept
        {
          return d != nullptr ? d->current.integer : fallback;
        }

        inline const char*
        read (dvar_t* d, const char* fallback) noexcept
        {
          return d != nullptr && d->current.string != nullptr
            ? d->current.string
            : fallback;
        }
      }
    }
  }
}
