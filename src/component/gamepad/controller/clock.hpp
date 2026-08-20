#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // The subsystem's single monotonic time source.
      //
      // Every timestamp the subsystem records Ã¢â‚¬â€ report acquisition, canonical
      // publication, engine consumption Ã¢â‚¬â€ is taken from this one clock so that any
      // two of them are directly comparable and latency is a plain subtraction.
      //
      // Note that XInput packet numbers are a change detector, not a timestamp. We
      // still timestamp the acquisition path from this clock so latency
      // measurements have a single source regardless of transport.
      //
      struct clock
      {
        using base       = chrono::steady_clock;
        using time_point = base::time_point;
        using duration   = base::duration;

        static_assert (base::is_steady,
                       "the subsystem clock must be monotonic; latency spans "
                       "assume acquisition never appears later than consumption");

        static time_point
        now () noexcept {return base::now ();}

        // Time elapsed since the subsystem's process epoch.
        //
        // Used by diagnostics to print short relative timestamps instead of raw
        // steady-clock counts. The epoch is fixed at first use.
        //
        static duration
        since_epoch () noexcept;
      };

      using timestamp = clock::time_point;

      // Explicit-unit durations for interfaces that carry controller timing.
      //
      // Using chrono types keeps frame time, poll intervals, and release delays
      // from being confused with plain integers or with the normalized scalars used
      // by the aim math.
      //
      using seconds      = chrono::duration<float>;
      using milliseconds = chrono::duration<float, std::milli>;

      // A pair of instants on the single clock bounding one sample's journey.
      //
      // latency () is non-negative provided consumed was taken no earlier than
      // acquired. Callers establish that ordering (both come from clock::now on the
      // same monotonic source) and may assert it; this type does not re-check it.
      //
      struct latency_span
      {
        timestamp acquired {};
        timestamp consumed {};

        clock::duration
        latency () const noexcept {return consumed - acquired;}
      };
    }
  }
}
