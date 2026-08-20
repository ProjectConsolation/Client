#include <std_include.hpp>

#include <component/gamepad/controller/engine/dvar.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        dvars
        register_dvars (const context& ctx)
        {
          dvars d;

          // Presence and enablement. present and in_use are read-only readouts the
          // subsystem writes; the rest are archived user settings.
          //
          d.enabled = Dvar_RegisterBool (
            "gpad_enabled", true, DVAR_ARCHIVE, "Game pad enabled");
          d.present = Dvar_RegisterBool (
            "gpad_present", false, DVAR_ROM, "A game pad is present");
          d.in_use = Dvar_RegisterBool (
            "gpad_in_use", false, DVAR_ROM, "A game pad is in use");
          d.rumble = Dvar_RegisterBool (
            "gpad_rumble", true, DVAR_ARCHIVE, "Enable game pad rumble");

          // PlayStation light bar, defaulting to MW2 menu gold. The channels are a
          // muted brass on a screen; a light bar is emissive, so a player may want to
          // brighten or saturate them, which is why they are exposed.
          //
          d.light_bar = Dvar_RegisterBool (
            "gpad_light_bar", true, DVAR_ARCHIVE,
            "Light the PlayStation controller's bar in the menu accent colour");
          d.light_bar_r = Dvar_RegisterInt (
            "gpad_light_bar_r", 196, 0, 255, DVAR_ARCHIVE, "Light bar red");
          d.light_bar_g = Dvar_RegisterInt (
            "gpad_light_bar_g", 151, 0, 255, DVAR_ARCHIVE, "Light bar green");
          d.light_bar_b = Dvar_RegisterInt (
            "gpad_light_bar_b", 54, 0, 255, DVAR_ARCHIVE, "Light bar blue");

          // Stick shaping.
          //
          d.stick_deadzone_min = Dvar_RegisterFloat (
            "gpad_stick_deadzone_min", 0.2f, 0.0f, 1.0f, DVAR_ARCHIVE,
            "Game pad inner stick deadzone");
          d.stick_deadzone_max = Dvar_RegisterFloat (
            "gpad_stick_deadzone_max", 0.01f, 0.0f, 1.0f, DVAR_ARCHIVE,
            "Game pad outer stick deadzone");
          d.button_deadzone = Dvar_RegisterFloat (
            "gpad_button_deadzone", 0.13f, 0.0f, 1.0f, DVAR_ARCHIVE,
            "Game pad trigger button deadzone");
          d.stick_pressed = Dvar_RegisterFloat (
            "gpad_stick_pressed", 0.4f, 0.0f, 1.0f, DVAR_ARCHIVE,
            "Deflection at which a stick counts as pressed");
          d.stick_pressed_hysteresis = Dvar_RegisterFloat (
            "gpad_stick_pressed_hysteresis", 0.1f, 0.0f, 1.0f, DVAR_ARCHIVE,
            "No-change band around the stick pressed threshold");

          // Named configurations.
          //
          d.buttons_config = Dvar_RegisterString (
            "gpad_buttonsConfig", "buttons_default_alt", DVAR_ARCHIVE,
            "Game pad button configuration");
          d.sticks_config = Dvar_RegisterString (
            "gpad_sticksConfig", "thumbstick_default", DVAR_ARCHIVE,
            "Game pad stick configuration");

          // View and aim.
          //
          d.invert_pitch = Dvar_RegisterBool (
            "input_invertPitch", false, DVAR_ARCHIVE, "Invert game pad pitch");
          d.view_sensitivity = Dvar_RegisterFloat (
            "input_viewSensitivity", 1.0f, 0.0001f, 5.0f, DVAR_ARCHIVE,
            "View sensitivity");
          d.sensitivity_level = Dvar_RegisterInt (
            "gpad_sensitivity", 5, 1, 10, DVAR_ARCHIVE,
            "Game pad look sensitivity (1-10)");
          d.aim_assist_enabled = Dvar_RegisterBool (
            "sv_allowAimAssist", true, DVAR_NONE,
            "Whether aim assist is enabled for clients");
          d.turnrate_pitch = Dvar_RegisterFloat (
            "aim_turnrate_pitch", 90.0f, 0.0f, 1080.0f, DVAR_CHEAT,
            "Hip vertical turn rate (deg/s)");
          d.turnrate_pitch_ads = Dvar_RegisterFloat (
            "aim_turnrate_pitch_ads", 55.0f, 0.0f, 1080.0f, DVAR_CHEAT,
            "ADS vertical turn rate (deg/s)");
          d.turnrate_yaw = Dvar_RegisterFloat (
            "aim_turnrate_yaw", 260.0f, 0.0f, 1080.0f, DVAR_CHEAT,
            "Hip horizontal turn rate (deg/s)");
          d.turnrate_yaw_ads = Dvar_RegisterFloat (
            "aim_turnrate_yaw_ads", 90.0f, 0.0f, 1080.0f, DVAR_CHEAT,
            "ADS horizontal turn rate (deg/s)");
          d.accel_enabled = Dvar_RegisterBool (
            "aim_accel_turnrate_enabled", true, DVAR_CHEAT,
            "Enable turn-rate acceleration");
          d.accel_rate = Dvar_RegisterFloat (
            "aim_accel_turnrate_lerp", 1200.0f, 0.0f, 4000.0f, DVAR_CHEAT,
            "Turn-rate acceleration (deg/s per second)");
          d.scale_view_axis = Dvar_RegisterBool (
            "aim_scale_view_axis", true, DVAR_CHEAT,
            "Scale the view axes by the dominant axis");

          // Assist behaviors.
          //
          d.slowdown_enabled = Dvar_RegisterBool (
            "aim_slowdown_enabled", true, DVAR_CHEAT, "Enable aim slowdown");
          d.lockon_enabled = Dvar_RegisterBool (
            "aim_lockon_enabled", false, DVAR_CHEAT, "Enable experimental lock-on aim assist");
          d.lockon_deflection = Dvar_RegisterFloat (
            "aim_lockon_deflection", 0.05f, 0.0f, 1.0f, DVAR_CHEAT,
            "Stick deflection at which lock-on activates");
          d.lockon_strength = Dvar_RegisterFloat (
            "aim_lockon_strengthYaw", 0.6f, 0.0f, 1.0f, DVAR_CHEAT,
            "Lock-on yaw assistance");
          d.lockon_pitch_strength = Dvar_RegisterFloat (
            "aim_lockon_strengthPitch", 0.0f, 0.0f, 1.0f, DVAR_CHEAT,
            "Lock-on pitch assistance");
          d.aim_assist_range_scale = Dvar_RegisterFloat (
            "aim_aimAssistRangeScale", 1.0f, 0.0f, 10.0f, DVAR_CHEAT,
            "Aim-assist target range scale");

          ctx.report (severity::info, facility::engine, errc::none,
                      "controller dvars registered");
          return d;
        }

        void
        publish_present (const dvars& d, bool present) noexcept
        {
          if (d.present != nullptr && d.present->current.enabled != present)
            Dvar_SetBool (d.present, present);
        }
      }
    }
  }
}
