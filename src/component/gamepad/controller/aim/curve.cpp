#include <std_include.hpp>

#include <component/gamepad/controller/aim/curve.hpp>

#include <cmath>
#include <algorithm>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        const char*
        to_string (curve_kind k) noexcept
        {
          switch (k)
          {
          case curve_kind::linear: return "linear";
          case curve_kind::power:  return "power";
          }

          return "linear";
        }

        bool
        validate (const response_curve& c, string& why) noexcept
        {
          if (c.kind == curve_kind::power)
          {
            if (!std::isfinite (c.exponent) || c.exponent <= 0.0f)
            {
              why = "power curve exponent must be finite and greater than zero";
              return false;
            }
          }

          return true;
        }

        float
        evaluate (const response_curve& c, float t) noexcept
        {
          t = std::clamp (t, 0.0f, 1.0f);

          switch (c.kind)
          {
          case curve_kind::linear:
            return t;
          case curve_kind::power:
            // 0 and 1 are fixed points of a power curve for any positive exponent, so
            // the range is preserved exactly at the endpoints.
            //
            return std::pow (t, c.exponent);
          }

          return t;
        }
      }
    }
  }
}
