#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        // An IW4x engine key for a controller input.
        //
        // The engine addresses controller inputs by key number. This is the boundary
        // type between the subsystem's physical inputs and the engine's binding and
        // key-event system. It deliberately carries no controller semantics of its own:
        // a key number is just a name the engine binds a command to. Physical buttons
        // map onto these, and these map onto commands, but the three layers stay
        // distinct so a key name never decides what a control means.
        //
        // 0xE0..0xF3 are the base engine's own numbers. APAD ("analog pad") is a stick
        // deflected far enough to read as a direction; the base engine gives both sticks
        // the same four APAD keys, which makes them impossible to bind apart. The right
        // stick therefore gets four keys of its own at 0xF4..0xF7. The engine's key-name
        // tables stop at 0xDE and its key state array holds 256 entries, so those numbers
        // are free and everything downstream Ã¢â‚¬â€ binding, dispatch, menus Ã¢â‚¬â€ is keynum
        // agnostic. APAD stays the left stick, so a configuration written before the
        // split keeps meaning what it meant.
        //
        enum class engine_key : int
        {
          button_a      = 0x01,
          button_b      = 0x02,
          button_x      = 0x03,
          button_y      = 0x04,
          button_lshldr = 0x05,
          button_rshldr = 0x06,
          button_start  = 0x0E,
          button_back   = 0x0F,
          button_lstick = 0x10,
          button_rstick = 0x11,
          button_ltrig  = 0x12,
          button_rtrig  = 0x13,
          dpad_up       = 0x14,
          dpad_down     = 0x15,
          dpad_left     = 0x16,
          dpad_right    = 0x17,

          // Left stick as a direction pad.
          //
          apad_up       = 0x1C,
          apad_down     = 0x1D,
          apad_left     = 0x1E,
          apad_right    = 0x1F,

          // Right stick as a direction pad.
          //
          rstick_up     = 0xE0,
          rstick_down   = 0xE1,
          rstick_left   = 0xE2,
          rstick_right  = 0xE3,
        };

        inline constexpr array<engine_key, 24> all_engine_keys {{
          engine_key::button_a, engine_key::button_b, engine_key::button_x,
          engine_key::button_y, engine_key::button_lshldr, engine_key::button_rshldr,
          engine_key::button_start, engine_key::button_back, engine_key::button_lstick,
          engine_key::button_rstick, engine_key::button_ltrig, engine_key::button_rtrig,
          engine_key::dpad_up, engine_key::dpad_down, engine_key::dpad_left,
          engine_key::dpad_right, engine_key::apad_up, engine_key::apad_down,
          engine_key::apad_left, engine_key::apad_right, engine_key::rstick_up,
          engine_key::rstick_down, engine_key::rstick_left, engine_key::rstick_right
        }};
        inline constexpr size_t engine_key_count {all_engine_keys.size ()};

        size_t key_index (engine_key) noexcept;

        // The engine's key state array is keys[256], and every key we synthesize indexes
        // it directly.
        //
        static_assert (static_cast<int> (engine_key::rstick_right) < 256,
                       "controller keys must fit the engine's key state array");

        // Whether a raw engine key number is one of the controller keys.
        //
        bool
        is_controller_key (int keynum) noexcept;

        // The engine's textual name for a key ("BUTTON_A", "APAD_RIGHT", ...).
        //
        // These are the names the engine's Key_KeynumToString path expects, so a
        // controller bind reads and writes identically to a keyboard bind.
        //
        const char*
        key_name (engine_key) noexcept;

        // Resolve a key from its engine name (case-insensitive) or its number.
        //
        // Return nullopt when the name or number is not a controller key, so the
        // caller falls back to the engine's own keyboard tables rather than
        // fabricating a key.
        //
        optional<engine_key>
        key_from_name (string_view) noexcept;

        optional<engine_key>
        key_from_keynum (int) noexcept;
      }
    }
  }
}
