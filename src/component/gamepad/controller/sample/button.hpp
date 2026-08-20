#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Canonical physical button across the supported families.
      //
      // This is the device-neutral physical set Ã¢â‚¬â€ the thing under the player's
      // finger, named by position rather than by any single vendor's label. The
      // translation from a physical button to a logical game action, and from a
      // logical action to an engine key, belongs to controller/mapping and never
      // happens here. Positional names (face_south and so on) are used precisely so
      // that the layer stays independent of the Xbox/PlayStation labelling that the
      // glyph layer later re-applies.
      //
      // Trigger fully-pressed states (l2/r2) are present as buttons so that edge
      // detection and digital binds work uniformly; the analog value lives in the
      // trigger sample.
      //
      enum class button : uint8_t
      {
        face_south,     // Xbox A     / Cross.
        face_east,      // Xbox B     / Circle.
        face_west,      // Xbox X     / Square.
        face_north,     // Xbox Y     / Triangle.

        dpad_up,
        dpad_down,
        dpad_left,
        dpad_right,

        l1,             // Left shoulder.
        r1,             // Right shoulder.
        l2,             // Left trigger, fully pressed.
        r2,             // Right trigger, fully pressed.
        l3,             // Left stick click.
        r3,             // Right stick click.

        start,          // Xbox Menu  / Options.
        back,           // Xbox View  / Create/Share.
        guide,          // Xbox/Guide / PS.

        touchpad,       // Touchpad click (DualShock 4 / DualSense).
        mute,           // Microphone mute (DualSense).

        edge_paddle_left,   // DualSense Edge rear left paddle.
        edge_paddle_right,  // DualSense Edge rear right paddle.
        edge_fn_left,       // DualSense Edge left function button (Fn1).
        edge_fn_right,      // DualSense Edge right function button (Fn2).

        count,
      };

      static_assert (static_cast<size_t> (button::count) <= 32,
                     "button_set stores buttons in a 32-bit mask");

      const char*
      to_string (button) noexcept;

      // Set of physical buttons held in one sample.
      //
      // A fixed 32-bit mask: allocation-free and trivially copyable so it costs
      // nothing on the per-frame path. Edge detection is expressed against a
      // previous set rather than stored per button, which keeps the sample itself
      // stateless.
      //
      class button_set
      {
      public:
        constexpr button_set () = default;

        constexpr bool
        down (button b) const noexcept
        {
          return (bits_ & mask (b)) != 0;
        }

        constexpr void
        set (button b, bool on) noexcept
        {
          if (on)
            bits_ |= mask (b);
          else
            bits_ &= ~mask (b);
        }

        constexpr bool
        any () const noexcept {return bits_ != 0;}

        constexpr uint32_t
        value () const noexcept {return bits_;}

        // Buttons that are down in this set but were up in prev.
        //
        constexpr button_set
        pressed_since (button_set prev) const noexcept
        {
          return from_bits (bits_ & ~prev.bits_);
        }

        // Buttons that are up in this set but were down in prev.
        //
        constexpr button_set
        released_since (button_set prev) const noexcept
        {
          return from_bits (prev.bits_ & ~bits_);
        }

        friend constexpr bool
        operator== (button_set, button_set) noexcept = default;

      private:
        static constexpr uint32_t
        mask (button b) noexcept
        {
          return uint32_t {1} << static_cast<uint32_t> (b);
        }

        static constexpr button_set
        from_bits (uint32_t b) noexcept
        {
          button_set s;
          s.bits_ = b;
          return s;
        }

        uint32_t bits_ {0};
      };
    }
  }
}
