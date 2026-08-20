#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>

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
        // Register the controller's console commands.
        //
        // Registration order is fixed. The engine command ABI invokes a plain function
        // with no user data, so a handler reaches the subsystem through a single
        // documented file-scope pointer set here; that pointer refers to the
        // process-lifetime runtime, not to any temporary. The command storage the engine
        // records is likewise process-lifetime.
        //
        void
        register_commands (const context&, runtime&);
      }
    }
  }
}
