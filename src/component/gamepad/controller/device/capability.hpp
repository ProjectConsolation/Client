#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // A single decoded device feature.
      //
      // Capabilities are facts a driver has established about a specific physical
      // device, not assumptions about a family. A feature bit is set only when the
      // driver can actually decode or drive it over the current transport; for
      // example a DualSense on Bluetooth advertises haptics differently than on
      // USB, and the driver records what it can genuinely do, not what the model
      // supports in principle.
      //
      enum class capability : uint32_t
      {
        none               = 0,

        // Input features.
        //
        gyroscope          = 1u << 0,
        accelerometer      = 1u << 1,
        touchpad           = 1u << 2,
        battery            = 1u << 3,
        microphone_button  = 1u << 4,  // DualSense mute button.
        back_buttons       = 1u << 5,  // DualSense Edge rear function buttons.

        // Output features.
        //
        rumble             = 1u << 8,  // Dual eccentric-mass motors.
        haptics            = 1u << 9,  // DualSense voice-coil haptics.
        adaptive_triggers  = 1u << 10, // DualSense trigger effect motors.
        light_bar          = 1u << 11, // RGB light bar.
        player_leds        = 1u << 12, // Player-indicator LEDs.
      };

      // Set of decoded device features.
      //
      // A thin, allocation-free wrapper over the underlying bit set. It exists so
      // that capability queries read as intent (caps.has (capability::rumble))
      // rather than as manual bit twiddling, and so the mask cannot be confused
      // with an arbitrary integer.
      //
      class capabilities
      {
      public:
        using rep = std::underlying_type_t<capability>;

        constexpr capabilities () = default;

        constexpr capabilities (capability c) noexcept
          : bits_ (static_cast<rep> (c)) {}

        constexpr bool
        has (capability c) const noexcept
        {
          return (bits_ & static_cast<rep> (c)) == static_cast<rep> (c) &&
                 c != capability::none;
        }

        constexpr capabilities&
        add (capability c) noexcept
        {
          bits_ |= static_cast<rep> (c);
          return *this;
        }

        constexpr capabilities&
        remove (capability c) noexcept
        {
          bits_ &= ~static_cast<rep> (c);
          return *this;
        }

        constexpr bool
        empty () const noexcept {return bits_ == 0;}

        constexpr rep
        value () const noexcept {return bits_;}

        friend constexpr bool
        operator== (capabilities, capabilities) noexcept = default;

        friend constexpr capabilities
        operator| (capabilities l, capabilities r) noexcept
        {
          capabilities c;
          c.bits_ = l.bits_ | r.bits_;
          return c;
        }

      private:
        rep bits_ {0};
      };

      constexpr capabilities
      operator| (capability l, capability r) noexcept
      {
        return capabilities (l) | capabilities (r);
      }

      ostream&
      operator<< (ostream&, capabilities);
    }
  }
}
