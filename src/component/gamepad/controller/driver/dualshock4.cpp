#include <std_include.hpp>

#include <component/gamepad/controller/driver/dualshock4.hpp>

#include <component/gamepad/controller/driver/decode.hpp>
#include <component/gamepad/controller/driver/output-report.hpp>
#include <component/gamepad/controller/driver/playstation.hpp>

#include <algorithm>

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
          // Framing constants (drivers/hid/hid-playstation.c). The common report
          // block is 32 bytes; it begins after the report id over USB and after the
          // report id plus a two-byte header over Bluetooth. The Bluetooth report
          // carries a trailing little-endian CRC-32 over everything before it.
          //
          constexpr uint8_t ds4_report_usb {0x01};
          constexpr uint8_t ds4_report_bt {0x11};
          constexpr size_t  ds4_size_usb {64};
          constexpr size_t  ds4_size_bt {78};
          constexpr size_t  ds4_common_usb {1};
          constexpr size_t  ds4_common_bt {3};
          constexpr size_t  ds4_max_touch_usb {3};
          constexpr size_t  ds4_max_touch_bt {4};
          constexpr uint8_t ds4_battery_full {11};

          // Resolve the framing for a report, returning the offset of the common
          // block and the maximum number of touch sub-frames. Validates the report id,
          // length, and (Bluetooth) CRC against the raw bytes before anything is
          // decoded; returns false to reject a report that does not match its framing
          // rather than risk associating it with the wrong layout.
          //
          bool
          resolve_frame (span<const byte> r,
                         connection link,
                         size_t& common,
                         size_t& max_touch) noexcept
          {
            if (link == connection::usb)
            {
              if (r.size () < ds4_size_usb || rd_u8 (r, 0) != ds4_report_usb)
                return false;

              common = ds4_common_usb;
              max_touch = ds4_max_touch_usb;
              return true;
            }

            if (link == connection::bluetooth)
            {
              if (r.size () < ds4_size_bt || rd_u8 (r, 0) != ds4_report_bt)
                return false;

              uint32_t crc (rd_le32 (r, ds4_size_bt - 4));
              if (!verify_ps_crc32 (ps_input_crc_seed,
                                    r.first (ds4_size_bt - 4), crc))
                return false;

              common = ds4_common_bt;
              max_touch = ds4_max_touch_bt;
              return true;
            }

            return false;
          }
        }

        bool
        decode_dualshock4 (span<const byte> r,
                           connection link,
                           raw_sample& raw,
                           canonical_sample& canonical) noexcept
        {
          size_t b (0);
          size_t max_touch (0);
          if (!resolve_frame (r, link, b, max_touch))
            return false;

          // Sticks. Offsets are relative to the common block: x, y, rx, ry.
          //
          const uint8_t lx (rd_u8 (r, b + 0));
          const uint8_t ly (rd_u8 (r, b + 1));
          const uint8_t rx (rd_u8 (r, b + 2));
          const uint8_t ry (rd_u8 (r, b + 3));

          raw.sticks[static_cast<size_t> (stick::left)]  = {lx, ly};
          raw.sticks[static_cast<size_t> (stick::right)] = {rx, ry};

          auto& left (canonical.sticks[static_cast<size_t> (stick::left)]);
          auto& right (canonical.sticks[static_cast<size_t> (stick::right)]);
          left = {};
          right = {};
          left.raw = {lx, ly};
          left.normalized = normalize_ps_stick (lx, ly);
          right.raw = {rx, ry};
          right.normalized = normalize_ps_stick (rx, ry);

          // Buttons. buttons[0] holds the hat and the four face buttons; buttons[1]
          // the shoulders, triggers, share/options, and stick clicks; buttons[2] the
          // PS and touchpad-click buttons (the remaining bits are a report counter).
          //
          const uint8_t b0 (rd_u8 (r, b + 4));
          const uint8_t b1 (rd_u8 (r, b + 5));
          const uint8_t b2 (rd_u8 (r, b + 6));

          button_set btns;
          btns.set (button::face_west,  (b0 & 0x10u) != 0);  // square
          btns.set (button::face_south, (b0 & 0x20u) != 0);  // cross
          btns.set (button::face_east,  (b0 & 0x40u) != 0);  // circle
          btns.set (button::face_north, (b0 & 0x80u) != 0);  // triangle
          apply_hat (btns, static_cast<uint8_t> (b0 & 0x0Fu));

          btns.set (button::l1,    (b1 & 0x01u) != 0);
          btns.set (button::r1,    (b1 & 0x02u) != 0);
          btns.set (button::l2,    (b1 & 0x04u) != 0);
          btns.set (button::r2,    (b1 & 0x08u) != 0);
          btns.set (button::back,  (b1 & 0x10u) != 0);       // share
          btns.set (button::start, (b1 & 0x20u) != 0);       // options
          btns.set (button::l3,    (b1 & 0x40u) != 0);
          btns.set (button::r3,    (b1 & 0x80u) != 0);

          btns.set (button::guide,    (b2 & 0x01u) != 0);    // PS
          btns.set (button::touchpad, (b2 & 0x02u) != 0);

          canonical.buttons = btns;
          raw.buttons = static_cast<uint32_t> (b0) |
                        (static_cast<uint32_t> (b1) << 8) |
                        (static_cast<uint32_t> (b2) << 16);

          // Analog triggers (z, rz).
          //
          const uint8_t z (rd_u8 (r, b + 7));
          const uint8_t rz (rd_u8 (r, b + 8));
          raw.triggers[static_cast<size_t> (trigger_side::left)]  = z;
          raw.triggers[static_cast<size_t> (trigger_side::right)] = rz;
          canonical.triggers[static_cast<size_t> (trigger_side::left)] =
            {z, static_cast<float> (z) / 255.0f};
          canonical.triggers[static_cast<size_t> (trigger_side::right)] =
            {rz, static_cast<float> (rz) / 255.0f};

          // Motion. Retained as raw device counts in the raw sample; the calibration
          // stage converts these into the canonical sample's physical units using the
          // device's calibration feature report.
          //
          motion_sample motion;
          motion.gyro.angular_velocity =
            {static_cast<float> (rd_le16s (r, b + 12)),
             static_cast<float> (rd_le16s (r, b + 14)),
             static_cast<float> (rd_le16s (r, b + 16))};
          motion.accel.acceleration =
            {static_cast<float> (rd_le16s (r, b + 18)),
             static_cast<float> (rd_le16s (r, b + 20)),
             static_cast<float> (rd_le16s (r, b + 22))};
          motion.device_timestamp = rd_le16 (r, b + 9);
          raw.motion = motion;

          // Battery. status[0] holds the capacity in its low nibble and the cable
          // state in bit 4; a capacity of 11 with the cable attached means full.
          //
          const uint8_t st0 (rd_u8 (r, b + 29));
          const uint8_t cap (static_cast<uint8_t> (st0 & 0x0Fu));
          const bool cable ((st0 & 0x10u) != 0);

          battery_state bat;
          if (cable && cap >= ds4_battery_full)
          {
            bat.state = battery_state::status::full;
            bat.percent = uint8_t {100};
          }
          else
          {
            bat.state = cable ? battery_state::status::charging
                              : battery_state::status::discharging;
            bat.percent = static_cast<uint8_t> (std::min<int> (cap * 10, 100));
          }
          canonical.battery = bat;
          raw.battery = bat;

          // Touchpad. The report carries several chronological sub-frames; a
          // per-engine-frame snapshot needs only the current contact state, so the
          // first sub-frame is decoded when the report declares at least one.
          //
          const uint8_t num_touch (rd_u8 (r, b + 32));
          if (num_touch >= 1 && num_touch <= max_touch)
          {
            auto decode_point = [&r] (size_t off) -> touch_point
            {
              ps_touch_point p (decode_touch_point (r.subspan (off, 4)));
              return {p.active, p.id, p.x, p.y};
            };

            touchpad tp;
            tp.points[0] = decode_point (b + 34);
            tp.points[1] = decode_point (b + 38);
            canonical.touch = tp;
            raw.touch = tp;
          }
          else
          {
            canonical.touch.reset ();
          }

          canonical.motion.reset ();  // Physical-unit motion is the calibration stage's.
          canonical.caps = capability::gyroscope | capability::accelerometer |
                           capability::touchpad | capability::battery |
                           capability::rumble | capability::light_bar;

          return true;
        }

        dualshock4_driver::
        dualshock4_driver (const context& ctx,
                           transport::hid_device& hid,
                           device_id device)
          : ctx_ (ctx), hid_ (hid), device_ (device), link_ (hid.link ())
        {
        }

        bool
        dualshock4_driver::
        poll (raw_sample& raw, canonical_sample& canonical) noexcept
        {
          // The device reports faster than the engine frames, so drain what has
          // arrived and keep the newest valid report: an engine frame wants the
          // controller's current position, not a queue of stale ones. The bound keeps
          // a device that reports pathologically fast from stalling the frame; leftover
          // reports are simply drained on the next one.
          //
          array<byte, ds4_size_bt> buf;
          bool decoded (false);

          for (size_t i (0); i != max_reports_per_poll; ++i)
          {
            optional<size_t> n (hid_.read (buf));

            if (!n || *n == 0)
              break;

            const span<const byte> r (buf.data (), *n);

            if (decode_dualshock4 (r, link_, raw, canonical))
              decoded = true;
            else if (minimal_bluetooth_report (r, link_))
            {
              // Not a malformed report: the device has not left minimal mode, which
              // enable_extended_reports () asks it to do at bind. There is nothing to
              // decode in it -- it carries no motion or touchpad data -- so drop it,
              // and say so once rather than once per report.
              //
              if (!minimal_reported_)
              {
                minimal_reported_ = true;

                ctx_.report (severity::info, facility::decode, errc::none, device_,
                             "DualShock 4 is still sending minimal Bluetooth reports "
                             "and produces no input; the controller did not switch to "
                             "its extended report");
              }
            }
            else
              ctx_.report (severity::warning, facility::decode, errc::report_malformed,
                           device_, "DualShock 4 report failed framing or CRC "
                                    "validation and was dropped");
          }

          return decoded;
        }

        void
        dualshock4_driver::
        submit (const output_request& request) noexcept
        {
          // Encode into a stack buffer sized for the larger (Bluetooth) report; the
          // encoder returns the actual length and rejects a request the device and link
          // cannot honor rather than emitting anything uncertain.
          //
          array<byte, ds4_output_bt_size> buf;

          const optional<size_t> n (
            encode_dualshock4_output (request, link_, buf));

          if (!n)
          {
            ctx_.report (severity::info, facility::driver, errc::output_rejected,
                         device_, "DualShock 4 driver ignores an output request it "
                                  "cannot encode for this connection");
            return;
          }

          if (!hid_.write (span<const byte> (buf.data (), *n)))
            ctx_.report (severity::warning, facility::driver, errc::output_rejected,
                         device_, "DualShock 4 output report write failed");
        }
      }
    }
  }
}
