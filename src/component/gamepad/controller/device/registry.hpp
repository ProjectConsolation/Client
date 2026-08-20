#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/device/connection.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Authoritative set of currently attached devices.
      //
      // The registry is the one place that knows which physical devices exist and
      // what stable id each one has. Discovery mutates it as devices arrive and
      // leave Ã¢â‚¬â€ driven by device-change events, not polling Ã¢â‚¬â€ while the engine
      // frame reads it. Those happen on different threads, so the registry is
      // internally synchronized; the lock is held only for the brief record
      // operations, never across decode or engine work, so device changes cannot
      // hitch a frame.
      //
      // It stores identity and attachment only. It does not create drivers, decode
      // reports, or hold input; those belong to the driver and sample layers.
      //
      class registry
      {
      public:
        explicit
        registry (const context&);

        // Record a discovered device and return its id.
        //
        // If a device with an equivalent binding is already present, its existing
        // id is returned and its identity and capabilities are refreshed rather
        // than a duplicate being created; discovery may legitimately re-observe the
        // same device. A fresh device is assigned the next id, which is never
        // reused within the process.
        //
        device_id
        add (device_identity,
             controller::transport_kind,
             connection link,
             capabilities,
             transport_binding);

        // Drop a device by id. Returns whether a device was removed.
        //
        bool
        remove (device_id);

        // Copy of the record for id, or nullopt if it is not present.
        //
        // Returns a copy rather than a pointer so the caller holds valid data after
        // the lock is released and cannot race a concurrent removal.
        //
        optional<device_connection>
        find (device_id) const;

        // Visit every record under the lock.
        //
        // Because the lock is held for the duration, the callback must not call back
        // into the registry (add, remove, find, size), or it will deadlock. Keep the
        // callback to reading each record.
        //
        void
        for_each (function_ref<void (const device_connection&)>) const;

        size_t
        size () const;

        // Counter of membership changes.
        //
        // Bumped when a device is added or removed, never when an existing record is
        // refreshed in place. It exists so the per-frame path can decide in a single
        // atomic load that the device set is unchanged, and skip the copying that
        // reconciling against it would otherwise cost every frame.
        //
        uint64_t
        generation () const noexcept {return generation_.load ();}

      private:
        const context&            ctx_;
        mutable mutex             mutex_;
        uint32_t                  next_ {1};
        atomic<uint64_t>          generation_ {0};
        vector<device_connection> devices_;
      };
    }
  }
}
