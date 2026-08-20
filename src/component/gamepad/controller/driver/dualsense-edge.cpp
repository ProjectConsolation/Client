#include <std_include.hpp>

#include <component/gamepad/controller/driver/dualsense-edge.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace driver
      {
        bool
        decode_dualsense_edge (span<const byte> report,
                               connection link,
                               raw_sample& raw,
                               canonical_sample& canonical) noexcept
        {
          return decode_dualsense (report, link, raw, canonical, true);
        }

        bool
        dualsense_edge_driver::
        poll (raw_sample& raw, canonical_sample& canonical) noexcept
        {
          return read_and_decode (raw, canonical, true);
        }
      }
    }
  }
}
