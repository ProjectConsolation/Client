#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/sample/axis.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        // What a stick axis does, independent of which stick produces it.
        //
        // This is the logical layer of the stick mapping: the physical axis (a stick
        // and a component) maps onto one of these, and only these reach the view and
        // movement code. Naming them keeps a layout from being a pair of booleans whose
        // meaning has to be recovered at each use.
        //
        enum class virtual_axis : uint8_t
        {
          side,     // Strafe left and right.
          forward,  // Move forward and back.
          yaw,      // Turn left and right.
          pitch,    // Look up and down.
        };

        const char*
        to_string (virtual_axis) noexcept;

        // The four stick configurations the game offers.
        //
        // Southpaw swaps the sticks. Legacy swaps the horizontal axes between them, so
        // the left stick turns while the right strafes; a player who learned on it
        // cannot be served by simply exchanging the sticks.
        //
        enum class stick_layout : uint8_t
        {
          standard,          // thumbstick_default
          southpaw,          // thumbstick_southpaw
          legacy,            // thumbstick_legacy
          legacy_southpaw,   // thumbstick_legacysouthpaw
        };

        const char*
        to_string (stick_layout) noexcept;

        // Resolve the value of gpad_sticksConfig. An unrecognized name is the standard
        // layout, which is what the base game does with an unknown configuration.
        //
        stick_layout
        stick_layout_from_name (string_view) noexcept;

        // Which virtual axis a given physical stick component drives under a layout.
        //
        virtual_axis
        axis_for (stick_layout, stick which, bool horizontal) noexcept;

        // The stick deflections a layout produces, already assigned to their meanings.
        //
        // Movement axes carry the raw deflection; the caller scales them into the
        // usercmd's byte range. View axes are what the aim processor consumes. Both are
        // normalized scalars, never angles.
        //
        struct resolved_axes
        {
          float side {0.0f};
          float forward {0.0f};
          float yaw {0.0f};
          float pitch {0.0f};
        };

        // Assign the two calibrated stick vectors onto the four virtual axes.
        //
        // The vectors are in canonical space, up and right positive. This is a pure
        // permutation with sign handling and no shaping: deadzone, response curve, and
        // turn rates all belong further along, and applying any of them here would make
        // a layout change alter the feel of a control rather than only its location.
        //
        resolved_axes
        resolve (stick_layout, stick_vector left, stick_vector right) noexcept;
      }
    }
  }
}
