#include <std_include.hpp>

#include <component/gamepad/controller/device/id.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      ostream&
      operator<< (ostream& os, device_id d)
      {
        // Render the null handle distinctly so diagnostics never read as if a real
        // device with id 0 existed.
        //
        if (!d)
          return os << "device(none)";

        return os << "device(" << d.value () << ')';
      }

      const char*
      to_string (transport_kind t) noexcept
      {
        switch (t)
        {
        case transport_kind::unknown:   return "unknown";
        case transport_kind::xinput:    return "xinput";
        case transport_kind::raw_input: return "raw-input";
        case transport_kind::hid:       return "hid";
        }

        return "unknown";
      }

      ostream&
      operator<< (ostream& os, transport_kind t)
      {
        return os << to_string (t);
      }

      const char*
      to_string (connection c) noexcept
      {
        switch (c)
        {
        case connection::unknown:     return "unknown";
        case connection::usb:         return "usb";
        case connection::bluetooth:   return "bluetooth";
        case connection::virtualized: return "virtualized";
        }

        return "unknown";
      }

      ostream&
      operator<< (ostream& os, connection c)
      {
        return os << to_string (c);
      }
    }
  }
}
