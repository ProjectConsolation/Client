#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/driver/driver.hpp>
#include <component/gamepad/controller/transport/hid.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Decode a DualSense input report into raw and canonical samples.
        //
        // Handles USB report 0x01 (64 bytes) and Bluetooth report 0x31 (78 bytes),
        // selected by link; the Bluetooth report's trailing CRC-32 is verified before
        // any field is read. Returns false and leaves the outputs untouched on any
        // framing, length, or checksum mismatch.
        //
        // When edge is true the four DualSense Edge rear buttons (buttons[2] bits
        // 4..7) are decoded as well and the back-buttons capability is recorded. On a
        // standard DualSense those bits are not the Edge buttons, so they are read
        // only when the caller has established the device is an Edge; this is how the
        // Edge driver adds its state without a second copy of the shared decode. The
        // DualSense and Edge share drivers/hid/hid-playstation.c's
        // struct dualsense_input_report.
        //
        bool
        decode_dualsense (span<const byte> report,
                          connection link,
                          raw_sample& raw,
                          canonical_sample& canonical,
                          bool edge = false) noexcept;

        // First-class DualSense driver.
        //
        // Decodes the DualSense's own model Ã¢â‚¬â€ adaptive-trigger-capable, haptics,
        // touchpad, motion, microphone button, battery, player LEDs, light bar Ã¢â‚¬â€
        // rather than reducing it to a common shape. Output (haptics, adaptive
        // triggers, LEDs) is withheld until its report encoder is verified.
        //
        class dualsense_driver: public driver
        {
        public:
          dualsense_driver (const context&, transport::hid_device&, device_id);

          controller::family
          family () const noexcept override {return controller::family::dualsense;}

          device_id
          device () const noexcept override {return device_;}

          bool
          poll (raw_sample&, canonical_sample&) noexcept override;

          void
          submit (const output_request&) noexcept override;

        protected:
          // Read one report and decode it, treating the device as a DualSense Edge
          // when edge is set. Shared by this driver and the Edge driver so the HID
          // read path exists once.
          //
          bool
          read_and_decode (raw_sample&, canonical_sample&, bool edge) noexcept;

          const context&         ctx_;
          transport::hid_device& hid_;
          device_id              device_;
          connection             link_;

          // The Bluetooth output-report sequence number. It wraps at 16 and is per
          // device, so the driver owns it; the encoder advances it on each Bluetooth
          // report. Unused over USB.
          //
          uint8_t bt_output_sequence_ {0};

          // Whether the device has been seen still sending minimal Bluetooth reports.
          // Reported once and then remembered: the device sends them at its full
          // report rate, so a diagnostic per report would bury the log.
          //
          bool minimal_reported_ {false};
        };
      }
    }
  }
}
