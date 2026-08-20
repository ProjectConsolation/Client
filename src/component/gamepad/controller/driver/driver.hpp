#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/device/id.hpp>
#include <component/gamepad/controller/device/identity.hpp>
#include <component/gamepad/controller/sample/sample.hpp>
#include <component/gamepad/controller/driver/output.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Upper bound on the reports a single poll () may consume from one device.
        //
        // HID controllers report at 250 Hz to 1 kHz, several times faster than the
        // engine frames, so a poll drains what has queued and keeps the newest reading
        // rather than advancing one report per frame and falling steadily behind. The
        // bound is what keeps a device that reports pathologically fast from turning
        // that drain into a stall; whatever is left over is drained next frame.
        //
        inline constexpr size_t max_reports_per_poll {32};

        // A bound native controller.
        //
        // One driver instance owns the decoding and output for one physical device
        // over one transport. Each driver decodes its own device's native report
        // model first Ã¢â‚¬â€ XInput state, a DualShock 4 HID report, a DualSense HID
        // report Ã¢â‚¬â€ and only then publishes a device-neutral canonical sample. Drivers
        // are never collapsed onto a common Xbox-shaped state before their
        // family-specific facts are decoded.
        //
        // Dynamic dispatch is used deliberately and only at this boundary. A driver is
        // polled at most a few times per engine frame Ã¢â‚¬â€ once per connected device,
        // not in a tight inner loop Ã¢â‚¬â€ so the indirect call is immeasurable next to a
        // single HID read or XInput poll. The hot per-sample work inside a driver uses
        // no virtual calls.
        //
        class driver
        {
        public:
          virtual
          ~driver () = default;

          // Which native family this driver decodes.
          //
          virtual controller::family
          family () const noexcept = 0;

          // The physical device this driver is bound to.
          //
          virtual device_id
          device () const noexcept = 0;

          // Acquire the next reading.
          //
          // Returns false when no new state is available Ã¢â‚¬â€ for XInput, when the packet
          // number is unchanged; for an event-driven HID device, when no report has
          // arrived. On true, both raw and canonical are filled: raw preserves the
          // device-native reading for calibration and diagnostics, canonical is the
          // game-facing state. The caller stamps the acquisition time and per-device
          // sequence and wraps canonical into a frame, keeping the clock single-
          // sourced. Must not throw across the ABI boundary and must not allocate.
          //
          virtual bool
          poll (raw_sample& raw, canonical_sample& canonical) noexcept = 0;

          // Apply an output request.
          //
          // The driver honors the requests its device and transport actually support
          // and reports the rest as output_rejected; it never emits a report it is not
          // certain of. Must not throw.
          //
          virtual void
          submit (const output_request&) noexcept = 0;
        };
      }
    }
  }
}
