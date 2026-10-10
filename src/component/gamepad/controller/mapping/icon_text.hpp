#pragma once
#include <array>
#include <cstddef>
#include <string_view>

namespace gamepad::unstable::controller::mapping::icon_text
{
  // QoS PC 1.1 RB_DrawText consumes eight bytes for an inline icon:
  // '^', type, width, height, four material-pointer bytes. Keep a NUL-free
  // private handle in text; the backend adapter resolves it to a trusted material.
  // Never serialize a process pointer into a localization string or a config.
  inline constexpr std::size_t button_count = 16;
  inline constexpr std::size_t count = button_count * 2;
  inline constexpr std::size_t token_size = 8;
  inline constexpr std::array<const char*, button_count> names{{
    "a", "b", "x", "y", "lb", "rb", "start", "back",
    "ls", "rs", "lt", "rt", "up", "down", "left", "right"
  }};

  inline constexpr auto tokens = []
  {
    std::array<std::array<char, token_size + 1>, count> result{};
    for (std::size_t i = 0; i < count; ++i)
      result[i] = {{'^', 1, 48, 48, 'C', 'G', 'I', static_cast<char>('A' + i), 0}};
    return result;
  }();

  inline int decode(std::string_view text) noexcept
  {
    if (text.size() < token_size || text[0] != '^' || text[1] != 1
        || text[2] != 48 || text[3] != 48 || text[4] != 'C'
        || text[5] != 'G' || text[6] != 'I') return -1;
    const auto index = static_cast<unsigned char>(text[7]) - static_cast<unsigned>('A');
    return index < count ? static_cast<int>(index) : -1;
  }

  // Only an entire binding-name token may bypass localization. Embedded icons,
  // normal localization keys and malformed handles retain the native path.
  inline bool is_binding_token(std::string_view text) noexcept
  {
    return text.size() == token_size && decode(text) >= 0;
  }

  // String dvar spelling is deliberately independent of device/input mapping.
  // Unknown values behave like auto; no device is guessed from its vendor name.
  inline int override_index(std::string_view value) noexcept
  {
    const auto equal = [value](std::string_view expected)
    {
      if (value.size() != expected.size()) return false;
      for (std::size_t i = 0; i < value.size(); ++i)
      {
        auto c = value[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c != expected[i]) return false;
      }
      return true;
    };
    return equal("xbox") ? 0 : equal("ps3") ? 1 : -1;
  }

  inline int advance(int pixel_height) noexcept { return (pixel_height * 32 + 16) / 32; }
}
