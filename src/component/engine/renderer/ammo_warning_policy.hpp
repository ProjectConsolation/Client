#pragma once

namespace ammo_warning
{
  // Called only after native visibility and low-clip checks accept the warning.
  // Empty clip WITH reserve must still show the binding-aware reload action.
  constexpr const char* no_reload_text(int clip, int reserve)
  {
    return reserve > 0 ? nullptr : clip > 0 ? "LOW AMMO" : "NO AMMO";
  }
}
