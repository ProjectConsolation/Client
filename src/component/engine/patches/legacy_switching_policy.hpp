#pragma once

namespace legacy_switching::policy
{
  constexpr bool sprint_animation(bool enabled, unsigned movement_flags)
  {
    return enabled && (movement_flags & 0x8000u) != 0;
  }

  constexpr bool cancelled_drop(unsigned old_state, unsigned current_weapon,
    unsigned requested_weapon, unsigned native_result_state)
  {
    return (old_state == 3 || old_state == 4) && current_weapon == requested_weapon
      && native_result_state == 0;
  }
}
