#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/device/id.hpp>
#include <component/gamepad/controller/transport/hid.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Switch a PlayStation controller reached over Bluetooth into extended-report
        // mode.
        //
        // Over Bluetooth both families power up in a minimal mode, emitting a 10-byte
        // 0x01 report that carries only the sticks and the buttons -- no gyroscope, no
        // accelerometer, no touchpad. They begin sending the full report the drivers
        // decode (DualSense 0x31, DualShock 4 0x11) after the host performs a feature
        // exchange. The payload is discarded; only the reporting-mode side effect is
        // wanted.
        //
        // Over USB the full report is sent from the start and this is unnecessary.
        //
        // Returns whether a feature report was read. A false return means the caller
        // must not bind a driver that only understands extended reports.
        //
        bool
        enable_extended_reports (const context&,
                                 transport::hid_device&,
                                 device_id,
                                 family) noexcept;

        // Whether a report is the minimal-mode Bluetooth report described above.
        //
        // Such a report is not malformed -- it is a mode the device has not left yet --
        // so a driver drops it without reporting a decode failure.
        //
        bool
        minimal_bluetooth_report (span<const byte>, connection link) noexcept;
      }
    }
  }
}
