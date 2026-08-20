#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <thread>
#include <atomic>

#include <component/gamepad/controller/context.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace transport
      {
        // Interrupt-driven notification of HID device arrival and removal.
        //
        // Windows delivers WM_DEVICECHANGE for a device-interface class only to a
        // window that has registered for it, so this owns a message-only window and a
        // thread to pump it. The thread does nothing but wait: it sets a flag when a
        // HID interface arrives or is removed and is otherwise asleep, so discovery no
        // longer has to poll to notice a controller being plugged in.
        //
        // The flag is the whole interface. It is deliberately coarse -- it says "the
        // set of devices may have changed", not which one -- because discovery already
        // reconciles the full set against the registry, and a precise notification
        // would only duplicate that work. A missed notification degrades to the poll
        // interval discovery keeps as a fallback, so the two are complementary rather
        // than a single point of failure.
        //
        class device_notifier
        {
        public:
          explicit
          device_notifier (const context&);

          ~device_notifier ();

          device_notifier (const device_notifier&) = delete;
          device_notifier& operator= (const device_notifier&) = delete;

          // Whether a device change has arrived since the last call. Clears the flag,
          // so a change is reported exactly once. Cheap enough to call every frame.
          //
          bool
          consume () noexcept {return pending_.exchange (false, std::memory_order_acquire);}

        private:
          void
          run (const context&) noexcept;

          std::atomic<bool>   pending_ {false};
          std::atomic<void*>  window_ {nullptr};  // HWND once the thread has created it.
          std::thread         thread_;
        };
      }
    }
  }
}
