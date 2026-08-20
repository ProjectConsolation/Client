#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/device/registry.hpp>
#include <component/gamepad/controller/driver/driver.hpp>
#include <component/gamepad/controller/transport/hid.hpp>
#include <component/gamepad/controller/transport/xinput-module.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        // The live drivers, one per registered device.
        //
        // This is where a registry record Ã¢â‚¬â€ identity and attachment Ã¢â‚¬â€ becomes a driver
        // that can actually be polled. It owns both the driver and, for HID devices,
        // the open transport the driver borrows, so that the two are created and
        // destroyed together and a driver can never outlive its handle.
        //
        // Binding is by family: an Xbox-class device gets the XInput driver, and each
        // PlayStation family gets its own. There is no fallback driver. A device whose
        // family or link the device layer could not establish is left unbound, because
        // the alternative is to decode its reports against a layout chosen by
        // elimination.
        //
        class set
        {
        public:
          set (const context&, const transport::xinput_module&);

          ~set ();

          set (const set&) = delete;
          set& operator= (const set&) = delete;

          // Create drivers for devices that have appeared and destroy drivers for
          // devices that have gone.
          //
          // Returns immediately when the registry's membership has not changed since
          // the last call, which is the steady state on every frame. The cost of that
          // check is one atomic load.
          //
          void
          reconcile (const registry&);

          // Visit each bound driver together with the device it drives.
          //
          void
          for_each (function_ref<void (driver&, const device_connection&)>);

          // Apply an output request to one device by id. Does nothing when the id is
          // not bound. The driver decides whether it can honor the request.
          //
          void
          submit (device_id, const output_request&);

          size_t
          size () const noexcept {return entries_.size ();}

        private:
          // One bound device. hid is null for XInput devices; when it is not, drv
          // holds a reference into it, so the two must be destroyed in this order and
          // the entry must not be copied.
          //
          struct entry
          {
            device_connection                device;
            unique_ptr<transport::hid_device> hid;
            unique_ptr<driver>               drv;
          };

          unique_ptr<driver>
          bind (const device_connection&, unique_ptr<transport::hid_device>&);

          const context&                  ctx_;
          const transport::xinput_module& xinput_;

          uint64_t       generation_ {0};
          bool           reconciled_ {false};
          vector<entry>  entries_;
        };
      }
    }
  }
}
