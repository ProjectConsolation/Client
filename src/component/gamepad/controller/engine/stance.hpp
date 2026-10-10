#pragma once
#include <cstdint>

namespace gamepad::unstable::controller::engine
{
  // KisakCOD CL_StanceButtonUpdate: a held stance button enters prone, or
  // stands when it was pressed while prone. QoS's tap/release path stays native.
  inline bool stance_hold_elapsed(std::uint32_t now, std::uint32_t pressed,
                                  int hold_ms) noexcept
  {
    return static_cast<std::uint32_t>(now - pressed) >=
      static_cast<std::uint32_t>(hold_ms < 0 ? 0 : hold_ms);
  }
  inline int held_stance(int starting_stance) noexcept
  { return starting_stance == 2 ? 0 : 2; }
}
