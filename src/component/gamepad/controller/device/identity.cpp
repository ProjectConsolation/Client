#include <std_include.hpp>

#include <component/gamepad/controller/device/identity.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      const char*
      to_string (family f) noexcept
      {
        switch (f)
        {
        case family::unknown:        return "unknown";
        case family::xbox:           return "xbox";
        case family::dualshock4:     return "dualshock4";
        case family::dualsense:      return "dualsense";
        case family::dualsense_edge: return "dualsense-edge";
        }

        return "unknown";
      }

      ostream&
      operator<< (ostream& os, family f)
      {
        return os << to_string (f);
      }

      family
      classify (vendor_id v, product_id p) noexcept
      {
        if (v == vendor_sony)
        {
          // Order matters: the Edge reports a distinct product id and must be
          // recognized before the generic DualSense so its extra state is not
          // erased by the DualSense driver.
          //
          if (p == product_dualsense_edge)
            return family::dualsense_edge;

          if (p == product_dualsense)
            return family::dualsense;

          if (p == product_ds4_gen1 ||
              p == product_ds4_gen2 ||
              p == product_ds4_dongle)
            return family::dualshock4;
        }

        // Microsoft-branded pads are driven through XInput rather than raw HID, so
        // they are classified by the XInput transport at discovery time, not here.
        // We still recognize the vendor for diagnostics but claim no HID family.
        //
        return family::unknown;
      }
    }
  }
}
