#include <std_include.hpp>

#include <component/gamepad/controller/driver/set.hpp>

#include <cassert>
#include <variant>
#include <algorithm>

#include <component/gamepad/controller/driver/xinput.hpp>
#include <component/gamepad/controller/driver/dualshock4.hpp>
#include <component/gamepad/controller/driver/dualsense.hpp>
#include <component/gamepad/controller/driver/dualsense-edge.hpp>
#include <component/gamepad/controller/driver/playstation.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        set::
        set (const context& ctx, const transport::xinput_module& x)
          : ctx_ (ctx), xinput_ (x)
        {
        }

        // Defined out of line so the driver and transport types need only be complete
        // in this translation unit.
        //
        set::
        ~set () = default;

        unique_ptr<driver>
        set::
        bind (const device_connection& d, unique_ptr<transport::hid_device>& hid)
        {
          switch (d.identity.family)
          {
          case family::xbox:
            {
              const auto* b (std::get_if<xinput_binding> (&d.binding));

              if (b == nullptr)
                break;

              return std::make_unique<xinput_driver> (ctx_, xinput_, d.id, b->index);
            }

          case family::dualshock4:
          case family::dualsense:
          case family::dualsense_edge:
            {
              if (hid == nullptr)
                break;

              // The link is what selects the report framing, and the device layer
              // never records a HID device whose link it could not establish.
              //
              assert (hid->link () == connection::usb ||
                      hid->link () == connection::bluetooth);

              // Over Bluetooth these pads power up sending a minimal report the
              // drivers have no layout for, and only start sending the extended one
              // once their calibration feature report has been read. Do that before
              // the driver is handed the device, so the first poll already has
              // something to decode. A pad that refuses the exchange only produces
              // minimal reports, which carry no usable input for these drivers, so
              // do not publish a driver that can never yield a frame.
              //
              if (hid->link () == connection::bluetooth &&
                  !enable_extended_reports (ctx_, *hid, d.id, d.identity.family))
                return nullptr;

              switch (d.identity.family)
              {
              case family::dualshock4:
                return std::make_unique<dualshock4_driver> (ctx_, *hid, d.id);

              case family::dualsense:
                return std::make_unique<dualsense_driver> (ctx_, *hid, d.id);

              case family::dualsense_edge:
                return std::make_unique<dualsense_edge_driver> (ctx_, *hid, d.id);

              default:
                break;
              }

              break;
            }

          case family::unknown:
            break;
          }

          return nullptr;
        }

        void
        set::
        reconcile (const registry& r)
        {
          const uint64_t g (r.generation ());

          if (reconciled_ && g == generation_)
            return;

          generation_ = g;
          reconciled_ = true;

          // Copy the current membership out from under the lock before doing anything
          // that could take it again, or that could be slow (opening a HID handle).
          //
          vector<device_connection> current;
          r.for_each ([&current] (const device_connection& d) {current.push_back (d);});

          // Retire drivers whose device is gone. The transport handle is released with
          // the driver that borrowed it.
          //
          std::erase_if (entries_, [this, &current] (const entry& e)
          {
            const bool gone (
              std::none_of (current.begin (), current.end (),
                            [&e] (const device_connection& d) {return d.id == e.device.id;}));

            if (gone)
              ctx_.report (severity::info, facility::driver, errc::none, e.device.id,
                           string ("driver released: ") +
                           to_string (e.device.identity.family));

            return gone;
          });

          // Bind drivers for devices that have appeared.
          //
          for (device_connection& d: current)
          {
            const bool bound (
              std::any_of (entries_.begin (), entries_.end (),
                           [&d] (const entry& e) {return e.device.id == d.id;}));

            if (bound)
              continue;

            unique_ptr<transport::hid_device> hid;

            if (const auto* b = std::get_if<hid_binding> (&d.binding))
            {
              hid = transport::open (ctx_, b->path);

              if (hid == nullptr)
                continue;  // open () reported the reason.
            }

            unique_ptr<driver> drv (bind (d, hid));

            if (drv == nullptr)
            {
              ctx_.report (severity::warning, facility::driver, errc::unsupported_device,
                           d.id,
                           string ("no driver binds ") + to_string (d.identity.family) +
                           " over " + to_string (d.transport));
              continue;
            }

            ctx_.report (severity::info, facility::driver, errc::none, d.id,
                         string ("driver bound: ") + to_string (d.identity.family) +
                         " over " + to_string (d.transport));

            entries_.push_back (entry {move (d), move (hid), move (drv)});
          }
        }

        void
        set::
        for_each (function_ref<void (driver&, const device_connection&)> fn)
        {
          for (entry& e: entries_)
            fn (*e.drv, e.device);
        }

        void
        set::
        submit (device_id id, const output_request& request)
        {
          for (entry& e: entries_)
          {
            if (e.device.id == id)
            {
              e.drv->submit (request);
              return;
            }
          }
        }
      }
    }
  }
}
