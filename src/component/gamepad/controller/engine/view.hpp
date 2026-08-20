#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include "component/gamepad/controller/engine/import.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/engine/dvar.hpp>
#include <component/gamepad/controller/aim/assist.hpp>
#include <component/gamepad/controller/aim/calibration.hpp>
#include <component/gamepad/controller/mapping/stick-layout.hpp>
#include <component/gamepad/controller/sample/sample.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        // Turns the controller's sticks into view and movement.
        //
        // This owns the second half of the input frame, the half the key dispatcher
        // does not: the analog sticks. The left stick (under the standard layout)
        // becomes forwardmove and rightmove on the usercmd; the right stick becomes a
        // view-angle change, run through the aim processor and the engine's own
        // aim-assist globals for slowdown and lock-on. Which stick does which is the
        // stick layout (gpad_sticksConfig).
        //
        // It holds the most recent shaped stick vectors, updated each input frame by
        // the runtime, and applies them when the engine builds a movement command. The
        // two happen at different points in the frame, so the sample is latched rather
        // than read live. The class carries the aim state (the turn integrators) across
        // frames, so it owns a persistent aim processor rebuilt only when the tuning
        // dvars change.
        //
        class view_driver
        {
        public:
          view_driver (const context&, const dvars&);

          // Latch this frame's sticks. Called from runtime::frame () with the active
          // device's calibrated sample; the components are assigned to virtual axes by
          // the configured layout and held until the movement command is built.
          //
          void
          observe (const canonical_sample&) noexcept;

          // No controller is driving this frame. Clears the latched deflection so a
          // stopped stick does not keep feeding movement, and resets the aim
          // integrators so acceleration does not carry across a gap.
          //
          void
          idle () noexcept;

          // Fold the controller's movement and view into a usercmd being built.
          //
          // This is the body of the CL_MouseMove replacement: it adds the movement
          // axes to forwardmove/rightmove and advances the view angles. frame_time is
          // the engine's frame duration in seconds. Left-handed weapons swap the attack
          // and throw buttons, matching the engine. Must not throw.
          //
          void
          apply_move (int client, usercmd_s& cmd, float frame_time) noexcept;

          // Fold the controller into a remote-control (killstreak) movement command.
          //
          void
          apply_remote_move (int client, usercmd_s& cmd) noexcept;

        private:
          // Rebuild the aim processor if a tuning dvar has changed since it was built.
          // Returns whether a processor is available.
          //
          bool
          ensure_processor ();

          // Whether the controller's view may move this frame: not while a menu, the
          // console, or the chat field holds input, and not while the player is frozen.
          //
          bool
          view_active (int client) const noexcept;

          const context& ctx_;
          const dvars&   dvars_;

          mapping::resolved_axes axes_ {};

          // The validated aim configuration and the processor built from it. The
          // configuration owns the response graph the processor points into, so the two
          // are held together and rebuilt together. Both are empty until the first
          // successful build; a build can fail on invalid user tuning, in which case the
          // view falls back to passing the stick through unshaped.
          //
          optional<aim::aim_calibration> calibration_;
          optional<aim::aim_processor>   processor_;

          // A cheap signature of the dvars the processor was built from, so a change is
          // noticed without rebuilding every frame.
          //
          size_t tuning_signature_ {0};
          bool   have_signature_ {false};
          bool   reported_invalid_ {false};
        };
      }
    }
  }
}
