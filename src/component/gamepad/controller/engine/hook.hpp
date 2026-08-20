#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      class runtime;
    }
  }
}

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        // Connects the engine's calls to the controller runtime.
        //
        // This is the only place engine addresses, detour metadata, and trampolines
        // live, so no address assumption leaks into the rest of the subsystem, and
        // every patch goes through the project's detour abstraction rather than
        // hand-written bytes.
        //
        // The QoS bridge supplies the PC key-name conversions, replaces CL_MouseMove
        // while a pad is driving, and extends the verified delta-usercmd callsites so
        // analog movement survives the network codec.
        //
        void
        install (runtime&);
      }
    }
  }
}
