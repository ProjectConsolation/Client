#include <std_include.hpp>

#include <component/gamepad/controller/device/capability.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      ostream&
      operator<< (ostream& os, capabilities caps)
      {
        // Render as a brace-enclosed, comma-separated list of the set features so a
        // diagnostic reads as a fact rather than a hexadecimal mask. The order is
        // fixed and independent of insertion order.
        //
        struct entry
        {
          capability bit;
          const char* name;
        };

        static constexpr entry entries[]
        {
          {capability::gyroscope,         "gyroscope"},
          {capability::accelerometer,     "accelerometer"},
          {capability::touchpad,          "touchpad"},
          {capability::battery,           "battery"},
          {capability::microphone_button, "microphone-button"},
          {capability::back_buttons,      "back-buttons"},
          {capability::rumble,            "rumble"},
          {capability::haptics,           "haptics"},
          {capability::adaptive_triggers, "adaptive-triggers"},
          {capability::light_bar,         "light-bar"},
          {capability::player_leds,       "player-leds"},
        };

        os << '{';

        bool first (true);
        for (const entry& e: entries)
        {
          if (!caps.has (e.bit))
            continue;

          if (!first)
            os << ", ";

          os << e.name;
          first = false;
        }

        return os << '}';
      }
    }
  }
}
