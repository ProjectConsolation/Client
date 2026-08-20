#include <std_include.hpp>

#include <component/gamepad/controller/device/registry.hpp>

#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      registry::
      registry (const context& ctx)
        : ctx_ (ctx)
      {
      }

      device_id
      registry::
      add (device_identity identity,
           controller::transport_kind t,
           connection link,
           capabilities caps,
           transport_binding binding)
      {
        device_id id;

        {
          lock_guard<mutex> l (mutex_);

          // Re-observing a present device refreshes its facts in place rather than
          // creating a duplicate. This branch releases the lock on return.
          //
          for (device_connection& d: devices_)
          {
            if (same_binding (d.binding, binding))
            {
              d.identity = identity;
              d.transport = t;
              d.link = link;
              d.caps = caps;
              return d.id;
            }
          }

          id = device_id (next_++);
          devices_.push_back (
            device_connection {id, identity, t, link, caps, move (binding)});

          generation_.fetch_add (1);
        }

        // Report the arrival with the lock released so a slow sink cannot stall a
        // concurrent frame reading the registry.
        //
        ctx_.report (severity::info, facility::discovery, errc::none, id,
                     string ("device connected: ") + to_string (identity.family) +
                     " over " + to_string (t) + '/' + to_string (link));
        return id;
      }

      bool
      registry::
      remove (device_id id)
      {
        controller::family family {controller::family::unknown};

        {
          lock_guard<mutex> l (mutex_);

          auto i (std::find_if (devices_.begin (), devices_.end (),
                                [id] (const device_connection& d)
                                {
                                  return d.id == id;
                                }));

          if (i == devices_.end ())
            return false;

          family = i->identity.family;
          devices_.erase (i);

          generation_.fetch_add (1);
        }

        ctx_.report (severity::info, facility::discovery, errc::none, id,
                     string ("device disconnected: ") + to_string (family));
        return true;
      }

      optional<device_connection>
      registry::
      find (device_id id) const
      {
        lock_guard<mutex> l (mutex_);

        for (const device_connection& d: devices_)
        {
          if (d.id == id)
            return d;
        }

        return nullopt;
      }

      void
      registry::
      for_each (function_ref<void (const device_connection&)> fn) const
      {
        lock_guard<mutex> l (mutex_);

        for (const device_connection& d: devices_)
          fn (d);
      }

      size_t
      registry::
      size () const
      {
        lock_guard<mutex> l (mutex_);
        return devices_.size ();
      }
    }
  }
}
