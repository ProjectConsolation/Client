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
        // glyphs and everything else to Xbox glyphs.
        //
        glyph_family
        glyph_family_for (controller::family device,
                          optional<glyph_family> user_override) noexcept;

        // The stable text label presented for a controller key.
        //
        // QoS does not ship every controller material used by later engine revisions,
        // so prompts use the serializable key names ("BUTTON_A", "DPAD_UP", ...)
        // instead of image control codes. The family parameter is retained for API
        // compatibility but does not alter the label.
        //
        const char*
        glyph_for (engine_key, glyph_family) noexcept;
      }
    }
  }
}
