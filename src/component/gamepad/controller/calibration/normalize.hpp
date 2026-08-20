#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/calibration/profile.hpp>
#include <component/gamepad/controller/sample/sample.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace calibration
      {
        // Apply a profile to a sample, producing its calibrated stage.
        //
        // Reads the driver's normalized sticks and triggers and the raw motion counts,
        // and writes: the calibrated stick vectors (centre and range corrected, radial
        // clamped, drift-thresholded), the calibrated trigger values (re-scaled to the
        // measured travel, in place of the driver's normalized ones), and the
        // physical-unit motion (debiased and scaled). It touches nothing the profile
        // does not describe Ã¢â‚¬â€ a sample without motion stays without motion.
        //
        // Deterministic and allocation-free. The profile is assumed valid (validate ()
        // has accepted it).
        //
        void
        apply (const profile&, const raw_sample&, canonical_sample&) noexcept;
      }
    }
  }
}
