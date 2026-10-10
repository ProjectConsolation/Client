#include <std_include.hpp>

#include <component/gamepad/controller/mapping/glyph.hpp>
#include <component/gamepad/controller/mapping/icon_text.hpp>

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
            return glyph_family::xbox;
          case controller::family::unknown:
            return glyph_family::playstation;
          }

          return glyph_family::playstation;
        }

        const char*
        glyph_for (engine_key k, glyph_family f) noexcept
        {
          // Presentation follows IW4x's key/family separation. Token encoding is
          // QoS-specific; IW4x's length-prefixed material names are not its ABI.
          for (size_t i = 0; i < icon_text::button_count; ++i)
            if (all_engine_keys[i] == k)
              return icon_text::tokens[i + (f == glyph_family::playstation
                ? icon_text::button_count : 0)].data();
          return nullptr;
        }
      }
    }
  }
}
