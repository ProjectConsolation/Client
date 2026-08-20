#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/device/id.hpp>
#include <component/gamepad/controller/device/identity.hpp>
#include <component/gamepad/controller/device/capability.hpp>

#include <variant>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // How to reach an XInput device: its user slot.
      //
      struct xinput_binding
      {
        user_index index;
      };

      // How to reach a HID device: the OS device interface path used to open it.
      //
      // The path, not an open handle, is the stable identity: it survives the
      // device being opened and closed, and two discovery passes that see the same
      // path are seeing the same physical device. The HID transport opens it
      // lazily.
      //
      struct hid_binding
      {
        wstring path;
      };

      // Transport-specific information needed to bind a driver to a device.
      //
      // monostate is the unbound state a record briefly holds before discovery
      // fills it in; a device is never handed to a driver factory in that state.
      //
      using transport_binding = variant<std::monostate,
                                        xinput_binding,
                                        hid_binding>;

      // Whether two bindings refer to the same physical attachment.
      //
      bool
      same_binding (const transport_binding&, const transport_binding&) noexcept;

      // A discovered physical device.
      //
      // This is the device layer's product: what the device is (identity), how it
      // is attached (transport, link, binding), and what it can do (capabilities).
      // It carries no decoded input and no report knowledge; turning reports into
      // samples is the driver layer's job. The id is assigned by the registry and
      // is stable for the device's presence.
      //
      struct device_connection
      {
        device_id                  id {};
        device_identity            identity {};
        controller::transport_kind transport {transport_kind::unknown};
        connection                 link {connection::unknown};
        capabilities               caps {};
        transport_binding          binding {};
      };
    }
  }
}
