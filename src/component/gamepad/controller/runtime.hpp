#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>
#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/diagnostic.hpp>

#include <component/gamepad/controller/device/id.hpp>
#include <component/gamepad/controller/device/registry.hpp>
#include <component/gamepad/controller/device/discovery.hpp>
#include <component/gamepad/controller/driver/set.hpp>
#include <component/gamepad/controller/transport/xinput-module.hpp>
#include <component/gamepad/controller/sample/frame.hpp>
#include <component/gamepad/controller/calibration/store.hpp>
#include <component/gamepad/controller/engine/dvar.hpp>
#include <component/gamepad/controller/engine/key.hpp>
#include <component/gamepad/controller/engine/bind.hpp>
#include <component/gamepad/controller/engine/view.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // The controller subsystem's lifetime and consistency invariant.
      //
      // The runtime is the single owner of everything the subsystem needs to be in
      // a correct state: the diagnostic sink, the XInput module, the device registry
      // and discovery, the bound native drivers, calibration, and the engine key and
      // bind bridges. Its existence is precisely the statement "the controller
      // subsystem is initialized and consistent."
      //
      // It is intentionally not a generic manager. It does not hold unrelated
      // services on behalf of others, and subsystems do not reach through it to
      // find one another; they receive a borrowed context (context.hxx) instead.
      // Ownership flows one way, from the runtime down.
      //
      // A single instance exists per process, created and destroyed by
      // mod::controller_module. It is non-copyable and non-movable because it owns
      // process-global engine hooks whose addresses must not be duplicated.
      //
      class runtime
      {
      public:
        explicit
        runtime (bool developer);

        ~runtime ();

        runtime (const runtime&) = delete;
        runtime& operator= (const runtime&) = delete;
        runtime (runtime&&) = delete;
        runtime& operator= (runtime&&) = delete;

        // Borrowed view of shared services for subsystems.
        //
        // Named make_context rather than context to avoid shadowing the context
        // type within the class scope; the value it returns is a cheap borrowed
        // view, not a fresh service.
        //
        context
        make_context () noexcept;

        diagnostic_sink&
        diagnostics () noexcept {return sink_;}

        // Register the engine dvars and commands, and take the bindings the
        // configured layout implies. Called once, on the first engine frame, by which
        // point the dvar and command systems exist.
        //
        void
        engine_ready ();

        // Advance the subsystem by one engine frame.
        //
        // Invoked from the engine's input frame through the engine hook surface. It
        // must not throw across the engine ABI boundary and must not allocate on the
        // per-frame acquisition path.
        //
        void
        frame ();

        // The device whose sample the engine is currently following, or no_device.
        //
        device_id
        active () const noexcept {return active_;}

        // The most recently published frame for the active device.
        //
        const input_frame&
        latest () const noexcept {return latest_;}

        engine::key_dispatcher&
        keys () noexcept {return keys_;}

        engine::bind_bridge&
        binds () noexcept {return binds_;}

        engine::view_driver&
        view () noexcept {return view_;}

        // Whether the controller is the active input source and enabled. The view and
        // movement hooks consult this to decide whether to replace the mouse path.
        //
        bool
        driving () const noexcept
        {
          return active_ != no_device && keys_.in_use () &&
                 engine::read (dvars_.enabled, true);
        }

        const engine::dvars&
        dvars () const noexcept {return dvars_;}

        size_t
        device_count () const noexcept {return drivers_.size ();}

      private:
        // Poll one driver and, when it produced a reading, fill out with the frame it
        // implies. Returns whether it did.
        //
        bool
        advance (driver::driver&, const device_connection&, input_frame& out);

        // The calibration profile for a family, loaded on first use.
        //
        // A profile is never shared across families: the store's file name encodes
        // the family and load () re-checks it, so a DualSense's measured centre can
        // never shape an Xbox pad's stick.
        //
        const calibration::profile&
        profile_for (controller::family);

        // Drive the PlayStation light bar to the configured colour. Sends an output
        // report only when the target device or colour has changed, so a held colour
        // is not re-transmitted every frame.
        //
        void
        apply_light_bar ();

        // Owned first so it is constructed before, and destroyed after, everything
        // that reports through it.
        //
        logging_sink sink_;
        bool         developer_;

        // Shared services, in construction order. Each borrows only what precedes it.
        //
        context                  ctx_;
        transport::xinput_module xinput_;
        registry                 devices_;
        discovery                discovery_;
        driver::set              drivers_;
        calibration::store       calibration_;

        engine::dvars          dvars_ {};
        engine::key_dispatcher keys_;
        engine::bind_bridge    binds_;
        engine::view_driver    view_;

        bool engine_ready_ {false};

        // One slot per family, indexed by its enumerator; empty until first use.
        //
        array<optional<calibration::profile>, 5> profiles_;

        static_assert (static_cast<size_t> (controller::family::dualsense_edge) == 4,
                       "profiles_ is indexed by family and must cover every one");

        // The device the engine follows, and the last frame it produced.
        //
        // One controller drives the game at a time: the engine's key state and view
        // are singular, so a second device's input would fight the first. The active
        // device is the one that most recently produced a reading, which is what
        // "the controller in the player's hands" means when several are attached.
        //
        device_id   active_ {};
        input_frame latest_ {};

        // Sample sequence numbers. Monotonically increasing across the subsystem, so
        // a consumer can order frames and notice a drop; last_published_ is what the
        // engine has actually seen.
        //
        uint64_t sequence_ {0};
        uint64_t last_published_ {0};

        // Set while a device was present on the previous frame, so its departure can
        // release any key it left held.
        //
        bool had_device_ {false};

        // The light-bar output last applied, so it is re-sent only on a change. The
        // key is the device and the four channel/enable values packed together; a
        // no_device key means nothing has been applied.
        //
        device_id lit_device_ {};
        uint32_t  lit_colour_ {0};
      };
    }
  }
}
