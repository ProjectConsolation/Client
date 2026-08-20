#include <std_include.hpp>

#include <component/gamepad/controller/driver/output-report.hpp>

#include <cassert>
#include <variant>
#include <algorithm>

#include <component/gamepad/controller/driver/decode.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        namespace
        {
          // A normalized magnitude in [0, 1] to a device byte.
          //
          uint8_t
          to_byte (float v) noexcept
          {
            return static_cast<uint8_t> (std::clamp (v, 0.0f, 1.0f) * 255.0f + 0.5f);
          }

          void
          put (span<byte> b, size_t i, uint8_t v) noexcept
          {
            assert (i < b.size ());
            b[i] = static_cast<byte> (v);
          }

          // Seal a Bluetooth output report: write the CRC-32 over everything before it,
          // computed with the output seed, into the last four bytes. Same construction
          // as the input CRC, only the seed differs (0xA2 rather than 0xA1).
          //
          void
          seal_bt_crc (span<byte> b) noexcept
          {
            assert (b.size () >= 4);

            const byte seed {static_cast<byte> (ps_output_crc_seed)};

            uint32_t crc (crc32_le (0xFFFFFFFFu, span<const byte> (&seed, 1)));
            crc = ~crc32_le (crc, b.subspan (0, b.size () - 4));

            const size_t o (b.size () - 4);
            put (b, o + 0, static_cast<uint8_t> (crc));
            put (b, o + 1, static_cast<uint8_t> (crc >> 8));
            put (b, o + 2, static_cast<uint8_t> (crc >> 16));
            put (b, o + 3, static_cast<uint8_t> (crc >> 24));
          }

          // DualShock 4 output-report valid-flag bits and the Bluetooth control byte,
          // from hid-playstation.c.
          //
          constexpr uint8_t ds4_flag0_motor {0x01};
          constexpr uint8_t ds4_flag0_led {0x02};
          constexpr uint8_t ds4_hwctl_crc32 {0x40};
          constexpr uint8_t ds4_hwctl_hid {0x80};

          // DualSense output-report valid-flag bits, from hid-playstation.c.
          //
          constexpr uint8_t ds_flag0_compatible_vibration {0x01};  // BIT(0)
          constexpr uint8_t ds_flag0_haptics_select {0x02};        // BIT(1)
          constexpr uint8_t ds_flag1_lightbar_enable {0x04};       // BIT(2)
          constexpr uint8_t ds_flag1_player_leds_enable {0x10};    // BIT(4)

          // DualSense Bluetooth output tag byte and the sequence-number field position.
          //
          constexpr uint8_t ds_output_tag {0x10};
        }

        optional<size_t>
        encode_dualshock4_output (const output_request& r,
                                  connection link,
                                  span<byte> out) noexcept
        {
          // The link decides the framing, and an ambiguous link has no correct framing
          // to choose, so it is refused rather than guessed.
          //
          const bool bt (link == connection::bluetooth);

          if (link != connection::usb && !bt)
            return nullopt;

          // The common block sits after the report id (USB) or after the report id and
          // two control bytes (Bluetooth); every field is written relative to it.
          //
          const size_t size (bt ? ds4_output_bt_size : ds4_output_usb_size);
          const size_t base (bt ? 3 : 1);

          if (out.size () < size)
            return nullopt;

          std::fill_n (out.data (), size, byte {});

          if (bt)
          {
            put (out, 0, 0x11);
            put (out, 1, ds4_hwctl_hid | ds4_hwctl_crc32);  // hw_control.
          }
          else
          {
            put (out, 0, 0x05);
          }

          if (const auto* rr = std::get_if<rumble_request> (&r))
          {
            // The common block is valid_flag0, valid_flag1, reserved, motor_right,
            // motor_left, so the motors sit at +3 and +4. strong (low-frequency) is
            // motor_left; weak (high-frequency) is motor_right, matching the kernel's
            // play_effect mapping.
            //
            put (out, base + 0, ds4_flag0_motor);  // valid_flag0.
            put (out, base + 3, to_byte (rr->high_frequency));  // motor_right.
            put (out, base + 4, to_byte (rr->low_frequency));   // motor_left.
          }
          else if (const auto* lb = std::get_if<light_bar_request> (&r))
          {
            put (out, base + 0, ds4_flag0_led);  // valid_flag0.
            put (out, base + 5, lb->red);
            put (out, base + 6, lb->green);
            put (out, base + 7, lb->blue);
          }
          else
          {
            // The DualShock 4 has no player-indicator LEDs, and adaptive triggers are
            // a DualSense feature; neither is encodable here.
            //
            return nullopt;
          }

          if (bt)
            seal_bt_crc (out.subspan (0, size));

          return size;
        }

        optional<size_t>
        encode_dualsense_output (const output_request& r,
                                 connection link,
                                 uint8_t& bt_sequence,
                                 span<byte> out) noexcept
        {
          const bool bt (link == connection::bluetooth);

          if (link != connection::usb && !bt)
            return nullopt;

          const size_t size (bt ? ds_output_bt_size : ds_output_usb_size);
          const size_t base (bt ? 3 : 1);

          if (out.size () < size)
            return nullopt;

          std::fill_n (out.data (), size, byte {});

          if (bt)
          {
            put (out, 0, 0x31);

            // The sequence number occupies the high nibble of seq_tag and wraps at 16;
            // tag is a fixed marker. Both are what the device expects to accept a
            // Bluetooth output report.
            //
            put (out, 1, static_cast<uint8_t> ((bt_sequence & 0x0F) << 4));
            put (out, 2, ds_output_tag);

            bt_sequence = static_cast<uint8_t> ((bt_sequence + 1) & 0x0F);
          }
          else
          {
            put (out, 0, 0x02);
          }

          // Common field offsets relative to base: valid_flag0 +0, valid_flag1 +1,
          // motor_right +2, motor_left +3, player_leds +43, lightbar_red +44,
          // lightbar_green +45, lightbar_blue +46.
          //
          if (const auto* rr = std::get_if<rumble_request> (&r))
          {
            // HAPTICS_SELECT routes the rumble to the voice-coil actuators; the
            // compatible-vibration flag drives them as classic rumble motors, which is
            // the broadly supported path.
            //
            put (out, base + 0, ds_flag0_haptics_select | ds_flag0_compatible_vibration);
            put (out, base + 2, to_byte (rr->high_frequency));  // motor_right.
            put (out, base + 3, to_byte (rr->low_frequency));   // motor_left.
          }
          else if (const auto* lb = std::get_if<light_bar_request> (&r))
          {
            put (out, base + 1, ds_flag1_lightbar_enable);  // valid_flag1.
            put (out, base + 44, lb->red);
            put (out, base + 45, lb->green);
            put (out, base + 46, lb->blue);
          }
          else if (const auto* pl = std::get_if<player_led_request> (&r))
          {
            put (out, base + 1, ds_flag1_player_leds_enable);  // valid_flag1.
            put (out, base + 43, pl->mask & 0x1F);             // player_leds.
          }
          else
          {
            // Adaptive-trigger effects are deliberately not encoded: the effect byte
            // layout is not covered by the kernel driver this file is verified against,
            // and an output report must never be guessed.
            //
            return nullopt;
          }

          if (bt)
            seal_bt_crc (out.subspan (0, size));

          return size;
        }
      }
    }
  }
}
