#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Which analog stick a reading belongs to.
      //
      enum class stick : uint8_t
      {
        left,
        right,
      };

      inline constexpr size_t stick_count {2};

      const char*
      to_string (stick) noexcept;

      // A stick position in normalized space.
      //
      // Both components lie in [-1, 1] after normalization; the magnitude may
      // exceed 1 only transiently before radial clamping. This is device-neutral
      // space: it says nothing about screen or world orientation, which the aim
      // layer introduces with its own units.
      //
      struct stick_vector
      {
        float x {0.0f};
        float y {0.0f};

        // Euclidean magnitude sqrt (x^2 + y^2).
        //
        float
        magnitude () const noexcept;
      };

      // Device-raw stick reading.
      //
      // The units are whatever the device report carries (for XInput, a signed
      // 16-bit count per axis). Preserved unmodified so calibration and diagnostics
      // can work from exactly what the hardware sent.
      //
      struct stick_raw
      {
        int32_t x {0};
        int32_t y {0};
      };

      // The four processing stages a stick carries through the pipeline.
      //
      // Keeping every stage lets calibration recompute from raw and lets the debug
      // tooling show precisely where a value changed. The game path consumes
      // filtered; everything before it is retained rather than discarded.
      //
      struct stick_sample
      {
        stick_raw    raw {};         // Exactly as decoded from the device report.
        stick_vector normalized {};  // Device-neutral, radial-clamped to [-1, 1].
        stick_vector calibrated {};  // Per-device center and range correction applied.
        stick_vector filtered {};    // Deadzone, curves, and smoothing applied; the
                                     // stage the game path reads. The aim layer owns
                                     // deadzone and curves; the driver fills only raw
                                     // and normalized.
      };
    }
  }
}
