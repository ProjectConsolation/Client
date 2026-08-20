#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>
#include <component/gamepad/controller/sample/sample.hpp>
#include <component/gamepad/controller/device/id.hpp>
#include <component/gamepad/controller/device/identity.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // One ordered, timestamped unit of input in the pipeline.
      //
      // A frame binds a canonical sample to the facts a consumer needs to order and
      // measure it: which device produced it, over which link, its per-device
      // sequence number, and the two instants on the single clock that bound its
      // journey. The sequence is monotonically increasing per device; the ring and
      // consumers rely on it to order frames and to detect drops. timing.acquired
      // is stamped by the producer; timing.consumed is stamped by the consumer when
      // it reads the frame, so latency () is meaningful only after consumption.
      //
      struct input_frame
      {
        device_id          device {};
        controller::family family {controller::family::unknown};
        connection         link {connection::unknown};
        uint64_t           sequence {0};
        latency_span       timing {};
        canonical_sample   state {};
      };

      ostream&
      operator<< (ostream&, const input_frame&);
    }
  }
}
