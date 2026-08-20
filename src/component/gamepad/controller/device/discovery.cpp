#include <std_include.hpp>

#include <component/gamepad/controller/device/discovery.hpp>

#include <cassert>
#include <algorithm>

#include <component/gamepad/controller/transport/hid.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace
      {
        constexpr capabilities dualsense_capabilities {
          capability::gyroscope |
          capability::accelerometer |
          capability::touchpad |
          capability::battery |
          capability::microphone_button |
          capability::rumble |
          capability::haptics |
          capability::adaptive_triggers |
          capability::light_bar |
          capability::player_leds};

        // What a family's device can do, as a fact about the hardware rather than
        // about what this subsystem currently drives. Output capabilities are
        // reported here even where the driver withholds the corresponding output
        // report, so a debug view and a future encoder agree on the device.
        //
        capabilities
        capabilities_for (family f) noexcept
        {
          switch (f)
          {
          case family::xbox:
            return capabilities (capability::rumble);

          case family::dualshock4:
            return capability::gyroscope |
                   capability::accelerometer |
                   capability::touchpad |
                   capability::battery |
                   capability::rumble |
                   capability::light_bar;

          case family::dualsense:
            return dualsense_capabilities;

          case family::dualsense_edge:
            return dualsense_capabilities | capability::back_buttons;

          case family::unknown:
            break;
          }

          return capabilities ();
        }
      }

      discovery::
      discovery (const context& ctx,
                 registry& r,
                 const transport::xinput_module& x)
        : ctx_ (ctx), registry_ (r), xinput_ (x), notifier_ (ctx)
      {
      }

      void
      discovery::
      scan ()
      {
        const bool changed (notifier_.consume ());
        const timestamp now (clock::now ());

        // A device-change notification forces a scan now; otherwise the interval
        // rate-limits it. The interval also catches XInput arrivals, which raise no
        // HID notification.
        //
        if (!changed && scanned_ && now - last_scan_ < interval)
          return;

        last_scan_ = now;
        scanned_ = true;

        scan_now ();
      }

      void
      discovery::
      scan_now ()
      {
        // Bindings observed in this pass. Sized for the four XInput slots plus a
        // handful of HID devices; growth beyond that is a reserve, not a bug.
        //
        vector<transport_binding> seen;
        seen.reserve (user_index::count + 4);

        scan_xinput (seen);

        // QoS has one local controller. When XInput is present, prefer it and do not
        // also open the native HID interface exposed by the same PlayStation pad
        // under DS4Windows. Besides producing duplicate input, that unnecessary HID
        // bind performs synchronous Bluetooth feature exchanges on the game thread.
        if (seen.empty ())
          scan_hid (seen);

        retire_unseen (seen);
      }

      void
      discovery::
      scan_xinput (vector<transport_binding>& seen)
      {
        if (!xinput_.loaded ())
          return;

        for (uint8_t i (0); i < user_index::count; ++i)
        {
          XINPUT_CAPABILITIES caps {};

          // XInputGetCapabilities, unlike XInputGetState, is the documented way to
          // ask whether a slot holds a device: it neither latches a packet number
          // nor is affected by the state cache. XINPUT_FLAG_GAMEPAD restricts the
          // answer to game pads, so a wheel or an arcade stick does not present
          // itself as a controller we know how to map.
          //
          if (xinput_.get_capabilities (i, XINPUT_FLAG_GAMEPAD, caps) !=
              ERROR_SUCCESS)
            continue;

          const user_index slot (i);

          // XInput does not report the physical link, and no XInput decode depends
          // on one: the state model is the same over USB and over the wireless
          // adaptor. The link is left unknown rather than guessed, which is
          // meaningful only for the HID drivers, whose report framing differs.
          //
          registry_.add (device_identity {family::xbox, nullopt, nullopt, nullopt},
                         transport_kind::xinput,
                         connection::unknown,
                         capabilities_for (family::xbox),
                         xinput_binding {slot});

          seen.push_back (xinput_binding {slot});
        }
      }

      void
      discovery::
      scan_hid (vector<transport_binding>& seen)
      {
        for (transport::hid_enumeration_entry& e: transport::enumerate (ctx_))
        {
          const family f (classify (e.attributes.vendor, e.attributes.product));

          // enumerate () filters to families we drive and to devices whose link it
          // could establish; both are preconditions for binding a driver.
          //
          assert (f != family::unknown);
          assert (e.link != connection::unknown);

          registry_.add (device_identity {f,
                                          e.attributes.vendor,
                                          e.attributes.product,
                                          e.attributes.version},
                         transport_kind::hid,
                         e.link,
                         capabilities_for (f),
                         hid_binding {e.path});

          seen.push_back (hid_binding {move (e.path)});
        }
      }

      void
      discovery::
      retire_unseen (const vector<transport_binding>& seen)
      {
        // Collect first, remove second: for_each holds the registry lock, and
        // remove () takes it.
        //
        vector<device_id> departed;

        registry_.for_each ([&seen, &departed] (const device_connection& d)
        {
          const bool present (
            std::any_of (seen.begin (), seen.end (),
                         [&d] (const transport_binding& b)
                         {
                           return same_binding (d.binding, b);
                         }));

          if (!present)
            departed.push_back (d.id);
        });

        for (device_id id: departed)
          registry_.remove (id);
      }
    }
  }
}
