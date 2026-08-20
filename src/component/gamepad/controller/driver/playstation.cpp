#include <std_include.hpp>

#include <component/gamepad/controller/driver/playstation.hpp>

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
          // The calibration feature report, which both families answer over Bluetooth
          // under the same id and size (Linux drivers/hid/hid-playstation.c:
          // DS_FEATURE_REPORT_CALIBRATION and DS4_FEATURE_REPORT_CALIBRATION_BT are
          // both 0x05, both 41 bytes -- the DualShock 4's USB-only 0x02/37 variant is
          // not needed here, since USB never has to be switched out of minimal mode).
          //
          constexpr uint8_t ps_feature_calibration {0x05};
          constexpr uint8_t ds_feature_serial {0x09};
          constexpr uint8_t ds_feature_firmware {0x20};
          constexpr size_t  ps_feature_calibration_size {41};

          // The report id both families use for the minimal Bluetooth report. The
          // extended reports the drivers decode are 0x31 (DualSense) and 0x11
          // (DualShock 4), so this id over Bluetooth is unambiguously the minimal one.
          //
          constexpr uint8_t ps_report_bt_minimal {0x01};

          // Upper bound on a feature report exchange, matching the transport's own read
          // buffer bound. The longest feature report either family declares is well
          // under this (the DualSense firmware-info report, at 64 bytes, is the
          // largest).
          //
          constexpr size_t max_feature_size {128};

          bool
          read_feature (transport::hid_device& hid, uint8_t report) noexcept
          {
            const size_t n (std::clamp (hid.feature_report_length (),
                                        ps_feature_calibration_size,
                                        max_feature_size));

            array<byte, max_feature_size> buf {};
            buf[0] = static_cast<byte> (report);
            return hid.get_feature (span<byte> (buf.data (), n));
          }
        }

        bool
        enable_extended_reports (const context& ctx,
                                 transport::hid_device& hid,
                                 device_id device,
                                 family device_family) noexcept
        {
          // Sony's Windows path uses the DualSense serial and firmware requests for
          // their side effect of enabling enhanced Bluetooth input. Calibration is a
          // useful fallback and remains the request used by DualShock 4.
          //
          bool enabled (false);

          if (device_family == family::dualsense ||
              device_family == family::dualsense_edge)
          {
            enabled = read_feature (hid, ds_feature_serial) ||
                      read_feature (hid, ds_feature_firmware) ||
                      read_feature (hid, ps_feature_calibration);
          }
          else
            enabled = read_feature (hid, ps_feature_calibration);

          if (!enabled)
            ctx.report (severity::warning, facility::transport,
                        errc::transport_failure, device,
                        "unable to enable extended Bluetooth reports; the controller "
                        "will remain inactive instead of publishing unusable input");

          return enabled;
        }

        bool
        minimal_bluetooth_report (span<const byte> r, connection link) noexcept
        {
          return link == connection::bluetooth &&
                 !r.empty () &&
                 rd_u8 (r, 0) == ps_report_bt_minimal;
        }
      }
    }
  }
}
