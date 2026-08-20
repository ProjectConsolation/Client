#include <std_include.hpp>

#include <component/gamepad/controller/driver/decode.hpp>

#include <cmath>
#include <cassert>
#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        uint8_t
        rd_u8 (span<const byte> d, size_t off) noexcept
        {
          assert (off < d.size ());
          return static_cast<uint8_t> (d[off]);
        }

        uint16_t
        rd_le16 (span<const byte> d, size_t off) noexcept
        {
          assert (off + 1 < d.size ());
          return static_cast<uint16_t> (
            static_cast<uint16_t> (d[off]) |
            (static_cast<uint16_t> (d[off + 1]) << 8));
        }

        int16_t
        rd_le16s (span<const byte> d, size_t off) noexcept
        {
          return static_cast<int16_t> (rd_le16 (d, off));
        }

        uint32_t
        rd_le32 (span<const byte> d, size_t off) noexcept
        {
          assert (off + 3 < d.size ());
          return static_cast<uint32_t> (d[off]) |
                 (static_cast<uint32_t> (d[off + 1]) << 8) |
                 (static_cast<uint32_t> (d[off + 2]) << 16) |
                 (static_cast<uint32_t> (d[off + 3]) << 24);
        }

        uint32_t
        crc32_le (uint32_t crc, span<const byte> data) noexcept
        {
          // Reflected bit-at-a-time CRC-32 (IEEE 802.3). A table would be faster, but a
          // Bluetooth report is at most 78 bytes and this runs once per report, so the
          // simpler form is kept for auditability against the polynomial.
          //
          // 0xEDB88320 is the bit reversal of the 0x04C11DB7 generator, which is what a
          // reflected implementation shifts right against. It is one bit away from
          // several plausible-looking constants, and a wrong one still produces
          // convincing-looking checksums that never match a device's, so it is pinned by
          // the standard check value in the tests rather than by inspection.
          //
          constexpr uint32_t reflected_polynomial {0xEDB88320u};

          for (byte b: data)
          {
            crc ^= static_cast<uint8_t> (b);

            for (int i (0); i < 8; ++i)
            {
              if ((crc & 1u) != 0)
                crc = (crc >> 1) ^ reflected_polynomial;
              else
                crc >>= 1;
            }
          }

          return crc;
        }

        bool
        verify_ps_crc32 (uint8_t seed,
                         span<const byte> data,
                         uint32_t expected) noexcept
        {
          const byte s {static_cast<byte> (seed)};

          uint32_t crc (crc32_le (0xFFFFFFFFu, span<const byte> (&s, 1)));
          crc = ~crc32_le (crc, data);

          return crc == expected;
        }

        ps_touch_point
        decode_touch_point (span<const byte> p) noexcept
        {
          assert (p.size () >= 4);

          const uint8_t contact (static_cast<uint8_t> (p[0]));
          const uint8_t x_lo (static_cast<uint8_t> (p[1]));
          const uint8_t mid (static_cast<uint8_t> (p[2]));
          const uint8_t y_hi (static_cast<uint8_t> (p[3]));

          ps_touch_point r {};
          r.active = (contact & 0x80u) == 0;
          r.id = static_cast<uint8_t> (contact & 0x7Fu);
          r.x = static_cast<uint16_t> (x_lo | ((mid & 0x0Fu) << 8));
          r.y = static_cast<uint16_t> ((mid >> 4) | (y_hi << 4));
          return r;
        }

        void
        apply_hat (button_set& s, uint8_t hat) noexcept
        {
          // Direction vectors for hat values 0..7; 8 (and anything else) is released.
          // Index order matches the device encoding: 0 is up, advancing clockwise.
          //
          struct dir { int x; int y; };
          static constexpr dir table[8]
          {
            { 0, -1}, // 0 up
            { 1, -1}, // 1 up-right
            { 1,  0}, // 2 right
            { 1,  1}, // 3 down-right
            { 0,  1}, // 4 down
            {-1,  1}, // 5 down-left
            {-1,  0}, // 6 left
            {-1, -1}, // 7 up-left
          };

          if (hat >= 8)
            return;

          const dir d (table[hat]);
          s.set (button::dpad_up,    d.y < 0);
          s.set (button::dpad_down,  d.y > 0);
          s.set (button::dpad_left,  d.x < 0);
          s.set (button::dpad_right, d.x > 0);
        }

        stick_vector
        normalize_ps_stick (uint8_t x, uint8_t y) noexcept
        {
          // Map the unsigned byte centred at 128 onto [-1, 1] using 127 as full scale
          // so that 255 reaches +1 and 1 reaches -1 symmetrically. Invert Y so up is
          // positive, matching the canonical convention.
          //
          float fx (std::clamp ((static_cast<float> (x) - 128.0f) / 127.0f,
                                -1.0f, 1.0f));
          float fy (std::clamp ((128.0f - static_cast<float> (y)) / 127.0f,
                                -1.0f, 1.0f));

          float m (std::sqrt (fx * fx + fy * fy));
          if (m > 1.0f)
          {
            fx /= m;
            fy /= m;
          }

          return {fx, fy};
        }
      }
    }
  }
}
