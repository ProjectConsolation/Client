#include <std_include.hpp>

#include <component/gamepad/controller/clock.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      clock::duration
      clock::
      since_epoch () noexcept
      {
        // Fix the epoch at first call. A function-local static gives us a
        // thread-safe, order-independent initialization without a separate
        // subsystem start hook: whichever component first consults the clock
        // establishes the reference instant.
        //
        static const time_point epoch (now ());
        return now () - epoch;
      }
    }
  }
}
