#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/driver/driver.hpp>
#include <component/gamepad/controller/transport/xinput-module.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // Decode an XInput gamepad state into raw and canonical samples.
        //
        // Exposed as a free function so the state conversion is unit-tested without a
        // live device or the loaded module. has_guide reports whether the guide button
        // bit is observable through the loaded runtime; when false the guide button is
        // left clear rather than reported as released.
        //
        void
        decode_xinput (const XINPUT_GAMEPAD&,
                       bool has_guide,
                       raw_sample&,
                       canonical_sample&) noexcept;

        // First-class XInput driver.
        //
        // Decodes the genuine XInput state model rather than a lowest-common-
        // denominator abstraction: packet numbers for change detection, separate
        // analog triggers, the guide button when the runtime exposes the extended
        // entry point, and dual-motor rumble output. XInput cannot faithfully
        // represent a DualShock or DualSense Ã¢â‚¬â€ it has no touchpad, motion, adaptive
        // triggers, or per-device HID facts Ã¢â‚¬â€ so this driver binds only to Xbox-class
        // devices. A PlayStation controller is decoded by its own driver and is never
        // funneled through XInput.
        //
        // The driver holds a borrowed reference to the shared XInput module and to one
        // user slot; it owns neither. Change detection is per instance, so two slots
        // never share packet-number state.
        //
        class xinput_driver: public driver
        {
        public:
          xinput_driver (const context&,
                         const transport::xinput_module&,
                         device_id,
                         user_index);

          controller::family
          family () const noexcept override {return controller::family::xbox;}

          device_id
          device () const noexcept override {return device_;}

          bool
          poll (raw_sample&, canonical_sample&) noexcept override;

          void
          submit (const output_request&) noexcept override;

        private:
          const context&                  ctx_;
          const transport::xinput_module& module_;
          device_id                       device_;
          user_index                      index_;

          uint32_t last_packet_ {0};
          bool      have_packet_ {false};
        };
      }
    }
  }
}
