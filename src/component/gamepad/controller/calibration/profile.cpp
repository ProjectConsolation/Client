#include <std_include.hpp>

#include <component/gamepad/controller/calibration/profile.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace calibration
      {
        const char*
        to_string (value_source s) noexcept
        {
          switch (s)
          {
          case value_source::built_in: return "built-in";
          case value_source::measured: return "measured";
          case value_source::user:     return "user";
          }

          return "built-in";
        }

        profile
        default_profile (controller::family f) noexcept
        {
          // The default is the identity in normalized space: it leaves the driver's
          // output unchanged, so a device is usable before it is measured. Motion is
          // passed through as raw counts (scale 1) because true physical units require
          // the device's measured calibration; a measured profile replaces these.
          //
          profile p;
          p.version = profile::current_version;
          p.family = f;
          p.source = value_source::built_in;

          for (stick_calibration& s: p.sticks)
            s = stick_calibration {};

          for (trigger_calibration& t: p.triggers)
            t = trigger_calibration {};

          p.motion = motion_calibration {};
          p.smoothing = 0.0f;
          return p;
        }
      }
    }
  }
}
