#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/clock.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // The aim math keeps physically distinct quantities in distinct types so they
        // cannot be silently interchanged. An angle is never assigned from a
        // normalized stick scalar, a turn rate is never treated as an angle, and a
        // screen-space vector never stands in for a world-space one. The types are
        // thin: each wraps one float and carries only the operations that are
        // dimensionally meaningful, so the compiler rejects the mistakes the old aim
        // code could make silently.

        inline constexpr float pi {3.14159265358979323846f};

        // An angle in degrees Ã¢â‚¬â€ the engine's native view-angle unit.
        //
        struct degrees
        {
          float value {0.0f};

          friend constexpr degrees
          operator+ (degrees a, degrees b) noexcept {return {a.value + b.value};}

          friend constexpr degrees
          operator- (degrees a, degrees b) noexcept {return {a.value - b.value};}

          friend constexpr degrees
          operator- (degrees a) noexcept {return {-a.value};}

          friend constexpr degrees
          operator* (degrees a, float s) noexcept {return {a.value * s};}

          friend constexpr bool
          operator== (degrees, degrees) noexcept = default;

          // A float is only partially ordered (NaN), so the ordering is a
          // partial_ordering; an explicit return type is required because a defaulted
          // friend comparison cannot deduce one.
          //
          friend constexpr std::partial_ordering
          operator<=> (degrees, degrees) noexcept = default;
        };

        // An angle in radians Ã¢â‚¬â€ used where trigonometry is clearer.
        //
        struct radians
        {
          float value {0.0f};
        };

        inline constexpr radians
        to_radians (degrees d) noexcept {return {d.value * (pi / 180.0f)};}

        inline constexpr degrees
        to_degrees (radians r) noexcept {return {r.value * (180.0f / pi)};}

        // An angular speed in degrees per second Ã¢â‚¬â€ a turn rate.
        //
        struct deg_per_s
        {
          float value {0.0f};

          friend constexpr deg_per_s
          operator* (deg_per_s r, float s) noexcept {return {r.value * s};}

          friend constexpr deg_per_s
          operator+ (deg_per_s a, deg_per_s b) noexcept {return {a.value + b.value};}

          friend constexpr bool
          operator== (deg_per_s, deg_per_s) noexcept = default;

          friend constexpr std::partial_ordering
          operator<=> (deg_per_s, deg_per_s) noexcept = default;
        };

        // Integrating a turn rate over a time step yields an angle.
        //
        inline constexpr degrees
        operator* (deg_per_s r, seconds dt) noexcept
        {
          return {r.value * dt.count ()};
        }

        // An angular acceleration in degrees per second squared Ã¢â‚¬â€ the rate at which a
        // turn rate is allowed to change (see the integrator).
        //
        struct deg_per_s2
        {
          float value {0.0f};
        };

        // Applied over a time step, an angular acceleration yields a change in turn
        // rate.
        //
        inline constexpr deg_per_s
        operator* (deg_per_s2 a, seconds dt) noexcept
        {
          return {a.value * dt.count ()};
        }

        // A signed normalized input along one axis, nominally in [-1, 1].
        //
        struct axis_input
        {
          float value {0.0f};

          constexpr float
          sign () const noexcept {return value >= 0.0f ? 1.0f : -1.0f;}

          constexpr float
          absolute () const noexcept {return value >= 0.0f ? value : -value;}
        };

        // A non-negative normalized magnitude, nominally in [0, 1].
        //
        struct magnitude
        {
          float value {0.0f};
        };

        // A vector in screen space (normalized device or pixel space, per the caller).
        //
        struct screen_vector
        {
          float x {0.0f};
          float y {0.0f};
        };

        // A vector in world space. Kept distinct from screen_vector so a projection or
        // a dot product cannot accidentally mix the two spaces.
        //
        struct world_vector
        {
          float x {0.0f};
          float y {0.0f};
          float z {0.0f};
        };

        inline constexpr float
        dot (world_vector a, world_vector b) noexcept
        {
          return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        ostream& operator<< (ostream&, degrees);
        ostream& operator<< (ostream&, deg_per_s);
      }
    }
  }
}
