#include <std_include.hpp>

#include <component/gamepad/controller/mapping/glyph.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        glyph_family
        glyph_family_for (controller::family device,
                          optional<glyph_family> user_override) noexcept
        {
          if (user_override)
            return *user_override;

          switch (device)
          {
          case controller::family::dualshock4:
          case controller::family::dualsense:
          case controller::family::dualsense_edge:
            return glyph_family::playstation;

          case controller::family::xbox:
          case controller::family::unknown:
            return glyph_family::xbox;
          }

          return glyph_family::xbox;
        }

        const char*
        glyph_for (engine_key k, glyph_family) noexcept
        {
          return key_name (k);
        }
      }
    }
  }
}
