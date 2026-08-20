#include <std_include.hpp>

#include <component/gamepad/controller/device/connection.hpp>

#include <variant>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      bool
      same_binding (const transport_binding& a, const transport_binding& b) noexcept
      {
        // Different alternatives cannot describe the same attachment.
        //
        if (a.index () != b.index ())
          return false;

        if (const auto* xa = std::get_if<xinput_binding> (&a))
          return xa->index == std::get<xinput_binding> (b).index;

        if (const auto* ha = std::get_if<hid_binding> (&a))
          return ha->path == std::get<hid_binding> (b).path;

        // Both are the unbound monostate; an unbound record identifies no device,
        // so two of them are not "the same device".
        //
        return false;
      }
    }
  }
}
