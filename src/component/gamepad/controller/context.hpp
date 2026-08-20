#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/error.hpp>
#include <component/gamepad/controller/diagnostic.hpp>
#include <component/gamepad/controller/device/id.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Shared services handed down to subsystems.
      //
      // The context is a borrowed view, not an owner: the runtime owns the concrete
      // services and passes a context by reference to the device, driver, sample,
      // aim, and engine layers so they can report diagnostics without each holding
      // its own sink or reaching back into the runtime. It carries no mutable state
      // of its own, which is why it is safe to hand the same context to every
      // subsystem.
      //
      // The clock is deliberately absent: it is a stateless monotonic source
      // (clock.hxx) that any component consults directly, so threading it through
      // the context would add coupling without adding a decision.
      //
      class context
      {
      public:
        explicit
        context (diagnostic_sink& sink, bool developer) noexcept
          : diagnostics_ (sink), developer_ (developer) {}

        diagnostic_sink&
        diagnostics () const noexcept {return diagnostics_;}

        // Whether the process is a developer build.
        //
        // Gates verbose reporting and developer-only tooling. It never changes
        // decoding or output behavior; a value that alters what the subsystem
        // decodes would belong in a driver or calibration decision, not here.
        //
        bool
        developer () const noexcept {return developer_;}

        // Report a structured diagnostic through the shared sink.
        //
        void
        report (severity, facility, errc, device_id, string message) const;

        void
        report (severity, facility, errc, string message) const;

      private:
        diagnostic_sink& diagnostics_;
        bool developer_;
      };
    }
  }
}
