#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/aim/types.hpp>
#include <component/gamepad/controller/aim/graph.hpp>
#include <component/gamepad/controller/aim/assist.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // Field-of-view aware turn-rate scale.
        //
        // Aim tuned at a reference FOV feels different at another FOV because the same
        // angular rate covers a different amount of the screen. Scaling the turn rate
        // by the ratio of the half-FOV tangents keeps on-screen speed consistent; the
        // result is 1 at the reference FOV. This is the standard model, and the
        // reference FOV is a tuning value rather than a hard constant.
        //
        float
        fov_scale (degrees fov, degrees reference_fov) noexcept;

        // Aim configuration as authored, before validation.
        //
        // The graph, when present, is given as its knots and is built and validated as
        // part of make (). graph_monotonic requires the built graph's outputs to be
        // non-decreasing.
        //
        struct aim_settings
        {
          aim_profile             hip;
          aim_profile             ads;
          turn_integrator::limits accel;
          optional<vector<knot>>  graph_knots;
          bool                    graph_monotonic {true};
        };

        // A validated aim configuration.
        //
        // make () validates every component as a unit Ã¢â‚¬â€ both deadzones, both curves,
        // and the aim graph Ã¢â‚¬â€ and rejects the whole configuration with a precise
        // reason if any part is invalid, so a bad value never reaches the per-frame
        // path. The built graph is owned here; a processor_config () refers to it, so
        // this object must outlive any processor built from it.
        //
        class aim_calibration
        {
        public:
          static optional<aim_calibration>
          make (const aim_settings&, string& why);

          aim_processor::config
          processor_config () const noexcept;

        private:
          aim_calibration () = default;

          aim_profile             hip_;
          aim_profile             ads_;
          turn_integrator::limits accel_;
          optional<aim_graph>     graph_;
        };
      }
    }
  }
}
