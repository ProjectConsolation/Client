#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/mapping/key.hpp>
#include <component/gamepad/controller/device/identity.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        // The controller family a glyph is drawn for.
        //
        // This is the family presented to the user, which is a rendering choice: a
        // user may prefer Xbox glyphs while holding a DualSense. Glyph selection
        // depends on this, never on the key name or the physical device, which is why
        // it lives in the mapping layer rather than the driver.
        //
        enum class glyph_family : uint8_t
        {
          xbox,
          playstation,
        };

        // Choose the glyph family to present for a physical device family, honoring a
        // user override when one is set. PlayStation devices default to PlayStation
        // glyphs; only XInput-class devices default to Xbox glyphs.
        //
        glyph_family
        glyph_family_for (controller::family device,
                          optional<glyph_family> user_override) noexcept;

        // NUL-free inline icon token for a button, or nullptr for stick directions.
        // Only presentation uses this; saved bindings continue to use key_name().
        //
        const char*
        glyph_for (engine_key, glyph_family) noexcept;
      }
    }
  }
}
