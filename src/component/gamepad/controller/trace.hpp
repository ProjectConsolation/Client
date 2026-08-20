#pragma once

// Subsystem profiling facade for the controller pipeline.
//
// The macros are kept, and compile to nothing.
//
// They used to expand to Tracy zones. Tracy is gone: its client queues an
// event per zone and only drains when a profiler server is attached, so a
// process with no profiler attached and no frame limiter -- a dedicated server
// is both -- grows without bound. That was measured at roughly 22 MB a second.
//
// The facade stays because the pipeline's stages are worth naming, and because
// the invariant it was written to check still matters: the per-frame
// acquisition contract has to hold whether or not instrumentation is compiled
// in. Anything put behind these macros must remain optional.

#ifndef LIBIW4X_CONTROLLER_TRACE
#  define LIBIW4X_CONTROLLER_TRACE 0
#endif

// Open a named zone for the enclosing scope.
//
#define CONTROLLER_ZONE(name) do {} while (false)

// Mark the boundary of one controller frame on the profiler timeline.
//
#define CONTROLLER_FRAME_MARK() do {} while (false)

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace trace
      {
        // Whether subsystem instrumentation is compiled in.
        //
        // Exposed as a typed constant so code and tests can branch on it
        // without repeating the preprocessor condition.
        //
        inline constexpr bool enabled {LIBIW4X_CONTROLLER_TRACE != 0};
      }
    }
  }
}
