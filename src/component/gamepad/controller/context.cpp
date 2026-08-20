#include <std_include.hpp>

#include <component/gamepad/controller/context.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      void
      context::
      report (severity level,
              facility origin,
              errc code,
              device_id device,
              string message) const
      {
        controller::report (diagnostics_, level, origin, code, device,
                            move (message));
      }

      void
      context::
      report (severity level,
              facility origin,
              errc code,
              string message) const
      {
        controller::report (diagnostics_, level, origin, code, move (message));
      }
    }
  }
}
