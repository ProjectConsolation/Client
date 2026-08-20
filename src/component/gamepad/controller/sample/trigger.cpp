#include <std_include.hpp>

#include <component/gamepad/controller/sample/trigger.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      const char*
      to_string (trigger_side s) noexcept
      {
        switch (s)
        {
        case trigger_side::left:  return "left";
        case trigger_side::right: return "right";
        }

        return "left";
      }
    }
  }
}
