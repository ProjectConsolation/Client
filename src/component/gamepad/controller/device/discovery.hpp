#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>
#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/device/registry.hpp>
#include <component/gamepad/controller/transport/xinput-module.hpp>
#include <component/gamepad/controller/transport/device-notify.hpp>
#include <atomic>
#include <thread>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Keeps the registry in step with the devices the operating system reports.
      //
      // Discovery is the only writer of the registry. It answers one question Ã¢â‚¬â€ what
      // is attached right now Ã¢â‚¬â€ and answers it from the transports themselves: the
      // four XInput user slots, and the present HID game pad interfaces whose USB
      // identifiers name a family we drive. It never opens a device for input, never
      // decodes a report, and never creates a driver.
      //
      // Enumerating devices is expensive relative to a frame, so a scan is driven
      // two ways: immediately when a HID device-change notification arrives, and
      // otherwise no more than once per interval. The notification makes a plugged-in
      // controller appear at once; the interval is the fallback that also catches
      // XInput arrivals, which raise no HID notification, and covers a notification
      // that was missed. The steady state is that scan () observes the same devices
      // and the registry's generation does not move, so nothing downstream works.
      //
      class discovery
      {
      public:
        discovery (const context&, registry&, const transport::xinput_module&);

        // Request worker reconciliation, at most once per interval. Enumeration
        // never blocks the engine frame. The synchronized registry publishes results.
        //
        void
        scan ();

        // Request reconciliation now, ignoring the interval. Completion is async.
        //
        void
        scan_now ();

        static constexpr clock::duration interval {
          chrono::milliseconds (1000)};

      private:
        void run(std::stop_token) noexcept;
        void reconcile_now();
        // Record the devices each transport currently reports, appending the binding
        // of every device seen to `seen`.
        //
        void
        scan_xinput (vector<transport_binding>& seen);

        void
        scan_hid (vector<transport_binding>& seen);

        // Drop registry records whose binding was not observed in this pass.
        //
        void
        retire_unseen (const vector<transport_binding>& seen);

        const context&                  ctx_;
        registry&                       registry_;
        const transport::xinput_module& xinput_;

        // Owns the message-only window that raises the flag consumed on the
        // engine thread. It outlives the enumeration worker during shutdown.
        //
        transport::device_notifier      notifier_;

        bool      scanned_ {false};
        timestamp last_scan_ {};
        // Last member: joins before notifier, registry, XInput and context die.
        // Adapted from IW4x Controller/Device/Discovery, no game ABI imports.
        std::atomic<bool> pending_ {false};
        std::jthread worker_;
      };
    }
  }
}
