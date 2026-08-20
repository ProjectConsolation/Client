#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        // One knot of a piecewise-linear response graph: an input fraction and the
        // output value it maps to.
        //
        struct knot
        {
          float input {0.0f};
          float output {0.0f};
        };

        // Upper bound on knots. A graph holds its knots inline, so evaluation and
        // storage are allocation-free.
        //
        inline constexpr size_t max_graph_knots {16};

        // A validated piecewise-linear response graph.
        //
        // A graph can only be created through make (), which validates the knots and
        // rejects invalid data with a precise reason. The accepted invariants are
        // that there are between two and max_graph_knots knots, that inputs lie in
        // [0, 1] and are strictly increasing, that every value is finite, and Ã¢â‚¬â€ when a
        // monotonic graph is required Ã¢â‚¬â€ that outputs are non-decreasing. Because a
        // graph cannot exist in an invalid state, evaluate () never validates; it
        // relies on those invariants (asserted) to stay deterministic and numerically
        // stable, since every segment has a strictly positive input span.
        //
        class aim_graph
        {
        public:
          // Build a graph, or return nullopt and set why on invalid data. Set
          // require_monotonic to reject a graph whose outputs decrease.
          //
          static optional<aim_graph>
          make (span<const knot>, bool require_monotonic, string& why);

          // Evaluate at input. Inputs at or beyond the end knots saturate to the end
          // outputs; an input exactly on a knot returns that knot's output exactly.
          //
          float
          evaluate (float input) const noexcept;

          span<const knot>
          knots () const noexcept {return {knots_.data (), knot_count_};}

          // Whether the outputs are non-decreasing across the graph.
          //
          bool
          monotonic () const noexcept {return monotonic_;}

        private:
          aim_graph () = default;

          array<knot, max_graph_knots> knots_ {};
          size_t                       knot_count_ {0};
          bool                         monotonic_ {false};
        };
      }
    }
  }
}
