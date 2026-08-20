#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/device/id.hpp>
#include <component/gamepad/controller/driver/output.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Encoders for the PlayStation output reports.
        //
        // Each function writes one complete output report into out and returns the byte
        // count written, or nullopt when the request is not something the family and
        // link support (an XInput-only rumble on a link with no encoder, say). The
        // buffer must be at least the report's size; the size is fixed per family and
        // link and is asserted.
        //
        // These are built against Linux drivers/hid/hid-playstation.c: the structure
        // layouts, the valid-flag bits that tell the device which fields to honor, and
        // the Bluetooth CRC and sequence tagging are reproduced from it. An output
        // report is never guessed -- an unrecognized request is rejected rather than
        // approximated -- because a malformed one can leave a controller in a bad
        // runtime state.
        //
        // The DualShock 4 and DualSense reports differ by link. USB carries the report
        // straight; Bluetooth prepends a control byte (DS4) or a sequence tag
        // (DualSense) and appends a CRC-32 over the whole report with the output seed.
        // The caller supplies the connection so the right framing is chosen; an
        // ambiguous connection has no encoder.

        // Report sizes, from hid-playstation.c.
        //
        inline constexpr size_t ds4_output_usb_size {32};
        inline constexpr size_t ds4_output_bt_size {78};
        inline constexpr size_t ds_output_usb_size {63};
        inline constexpr size_t ds_output_bt_size {78};

        // The Bluetooth output-report sequence number the DualSense tags each report
        // with. It wraps at 16 and is per device, so the driver owns one and passes it
        // in; the encoder increments the caller's counter.
        //
        optional<size_t>
        encode_dualshock4_output (const output_request&,
                                  connection,
                                  span<byte> out) noexcept;

        optional<size_t>
        encode_dualsense_output (const output_request&,
                                 connection,
                                 uint8_t& bt_sequence,
                                 span<byte> out) noexcept;
      }
    }
  }
}
