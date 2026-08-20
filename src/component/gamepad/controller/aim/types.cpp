#include <std_include.hpp>

#include <component/gamepad/controller/aim/types.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        ostream&
        operator<< (ostream& os, degrees d)
        {
          return os << d.value << "deg";
        }

        ostream&
        operator<< (ostream& os, deg_per_s r)
        {
          return os << r.value << "deg/s";
        }
      }
    }
  }
}
