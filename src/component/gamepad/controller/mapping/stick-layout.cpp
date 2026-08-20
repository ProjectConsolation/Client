#include <std_include.hpp>

#include <component/gamepad/controller/mapping/stick-layout.hpp>

#include <cctype>
#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        namespace
        {
          string
          lowercase (string_view s)
          {
            string r (s);

            std::transform (r.begin (), r.end (), r.begin (), [] (char c)
            {
              return static_cast<char> (
                std::tolower (static_cast<unsigned char> (c)));
            });

            return r;
          }

          // Movement axes are scaled by the deflection of the stick that produces
          // them, which is what the base game calls a squared mapping: pushing a stick
          // to a corner commands full speed in both directions, while a half push in
          // one direction commands a quarter of it. Because a stick vector is already
          // radially clamped, the factor is simply its magnitude.
          //
          // View axes take the deflection unchanged. Shaping them is the aim layer's
          // job, and doing any of it here would make choosing a layout change how a
          // control feels rather than only where it is.
          //
          float
          squared (float component, stick_vector v) noexcept
          {
            return std::clamp (v.magnitude () * component, -1.0f, 1.0f);
          }
        }

        const char*
        to_string (virtual_axis a) noexcept
        {
          switch (a)
          {
          case virtual_axis::side:    return "side";
          case virtual_axis::forward: return "forward";
          case virtual_axis::yaw:     return "yaw";
          case virtual_axis::pitch:   return "pitch";
          }

          return "unknown";
        }

        const char*
        to_string (stick_layout l) noexcept
        {
          switch (l)
          {
          case stick_layout::standard:        return "thumbstick_default";
          case stick_layout::southpaw:        return "thumbstick_southpaw";
          case stick_layout::legacy:          return "thumbstick_legacy";
          case stick_layout::legacy_southpaw: return "thumbstick_legacysouthpaw";
          }

          return "thumbstick_default";
        }

        stick_layout
        stick_layout_from_name (string_view name) noexcept
        {
          const string n (lowercase (name));

          // Test the compound name before its constituents: legacysouthpaw contains
          // both "legacy" and "southpaw".
          //
          if (n == "thumbstick_legacysouthpaw")
            return stick_layout::legacy_southpaw;

          if (n == "thumbstick_legacy")
            return stick_layout::legacy;

          if (n == "thumbstick_southpaw")
            return stick_layout::southpaw;

          return stick_layout::standard;
        }

        virtual_axis
        axis_for (stick_layout l, stick which, bool horizontal) noexcept
        {
          // Southpaw exchanges which stick moves and which looks. Legacy additionally
          // exchanges the two horizontal axes, so the movement stick turns and the view
          // stick strafes.
          //
          const bool southpaw (l == stick_layout::southpaw ||
                               l == stick_layout::legacy_southpaw);
          const bool legacy (l == stick_layout::legacy ||
                             l == stick_layout::legacy_southpaw);

          const bool move_stick ((which == stick::left) != southpaw);

          if (!horizontal)
            return move_stick ? virtual_axis::forward : virtual_axis::pitch;

          // Horizontally, legacy hands the movement stick the turn and the view stick
          // the strafe.
          //
          if (move_stick)
            return legacy ? virtual_axis::yaw : virtual_axis::side;

          return legacy ? virtual_axis::side : virtual_axis::yaw;
        }

        resolved_axes
        resolve (stick_layout l, stick_vector left, stick_vector right) noexcept
        {
          resolved_axes r;

          const stick sticks[] {stick::left, stick::right};

          for (stick s: sticks)
          {
            const stick_vector& v (s == stick::left ? left : right);

            for (bool horizontal: {true, false})
            {
              const float component (horizontal ? v.x : v.y);

              switch (axis_for (l, s, horizontal))
              {
              case virtual_axis::side:    r.side = squared (component, v);    break;
              case virtual_axis::forward: r.forward = squared (component, v); break;
              case virtual_axis::yaw:     r.yaw = component;                  break;
              case virtual_axis::pitch:   r.pitch = component;                break;
              }
            }
          }

          return r;
        }
      }
    }
  }
}
