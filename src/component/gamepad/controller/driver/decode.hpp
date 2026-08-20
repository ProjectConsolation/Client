#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/sample/axis.hpp>
#include <component/gamepad/controller/sample/button.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Little-endian field readers over a report buffer.
        //
        // Decoders read every wire field through these so the extraction is explicit
        // and host-endianness- and alignment-independent: HID reports are packed and
        // a multi-byte field is frequently unaligned. The offset is asserted in
        // bounds; a decoder validates the report length against external data before
        // reading, then these document the invariant that the bytes are present.
        //
        uint8_t
        rd_u8 (span<const byte>, size_t offset) noexcept;

        uint16_t
        rd_le16 (span<const byte>, size_t offset) noexcept;

        int16_t
        rd_le16s (span<const byte>, size_t offset) noexcept;

        uint32_t
        rd_le32 (span<const byte>, size_t offset) noexcept;

        // Standard reflected CRC-32 (polynomial 0xEDB88820).
        //
        // Bit-for-bit identical to the Linux crc32_le the PlayStation HID drivers use:
        // running value in, no implicit pre- or post-inversion. The caller supplies
        // the initial value and applies the final complement (see verify_ps_crc32).
        //
        uint32_t
        crc32_le (uint32_t init, span<const byte>) noexcept;

        // Seeds prepended before the payload when computing a PlayStation report CRC.
        //
        inline constexpr uint8_t ps_input_crc_seed {0xA1};
        inline constexpr uint8_t ps_output_crc_seed {0xA2};
        inline constexpr uint8_t ps_feature_crc_seed {0xA3};

        // Verify a PlayStation Bluetooth report CRC exactly as hid-playstation.c does:
        //
        //   crc = crc32_le (0xFFFFFFFF, {seed});
        //   crc = ~crc32_le (crc, data);
        //   return crc == expected;
        //
        // data is the report bytes excluding the trailing four CRC bytes.
        //
        bool
        verify_ps_crc32 (uint8_t seed, span<const byte> data, uint32_t expected) noexcept;

        // A decoded touchpad contact from a 4-byte PlayStation touch point.
        //
        struct ps_touch_point
        {
          bool     active;
          uint8_t  id;
          uint16_t x;  // 12-bit device coordinate.
          uint16_t y;  // 12-bit device coordinate.
        };

        // Decode the 4-byte touch point at p: contact, x_lo, (x_hi:4 | y_lo:4), y_hi.
        //
        // x = x_lo | (x_hi << 8); y = y_lo | (y_hi << 4); the contact byte's high bit
        // marks the point inactive and the low seven bits are its tracking id.
        //
        ps_touch_point
        decode_touch_point (span<const byte> p) noexcept;

        // Apply a PlayStation hat value (0..8) to the dpad buttons of a set.
        //
        // The hat encodes eight directions plus a released state (8); diagonals set
        // two dpad buttons. Values outside 0..7 leave the dpad clear.
        //
        void
        apply_hat (button_set&, uint8_t hat) noexcept;

        // Normalize a PlayStation stick byte pair into canonical space.
        //
        // The sticks are unsigned bytes centred at 128. The result matches the
        // canonical convention used by every driver Ã¢â‚¬â€ up and right are positive, the
        // same as XInput Ã¢â‚¬â€ and is radially clamped to unit magnitude. The device's
        // Y axis grows downward, so it is inverted here.
        //
        stick_vector
        normalize_ps_stick (uint8_t x, uint8_t y) noexcept;
      }
    }
  }
}
