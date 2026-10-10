#pragma once
#include <component/gamepad/controller/mapping/glyph.hpp>

namespace gamepad::unstable::controller
{
  class runtime;
  namespace engine
  {
    mapping::glyph_family displayed_icon_family(const runtime&) noexcept;
    const char* displayed_button_icon(const runtime&, mapping::engine_key) noexcept;
    void install_prompt_icons();
  }
}
