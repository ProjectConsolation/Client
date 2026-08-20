#include <std_include.hpp>

#include <component/gamepad/controller/aim/graph.hpp>

#include <cmath>
#include <cassert>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace aim
      {
        optional<aim_graph>
        aim_graph::
        make (span<const knot> ks, bool require_monotonic, string& why)
        {
          if (ks.size () < 2)
          {
            why = "an aim graph requires at least two knots";
            return nullopt;
          }

          if (ks.size () > max_graph_knots)
          {
            why = "an aim graph has too many knots";
            return nullopt;
          }

          bool non_decreasing (true);

          for (size_t i (0); i < ks.size (); ++i)
          {
            if (!std::isfinite (ks[i].input) || !std::isfinite (ks[i].output))
            {
              why = "aim graph knot values must be finite";
              return nullopt;
            }

            if (ks[i].input < 0.0f || ks[i].input > 1.0f)
            {
              why = "aim graph knot inputs must lie in [0, 1]";
              return nullopt;
            }

            if (i > 0)
            {
              if (ks[i].input <= ks[i - 1].input)
              {
                why = "aim graph knot inputs must be strictly increasing";
                return nullopt;
              }

              if (ks[i].output < ks[i - 1].output)
                non_decreasing = false;
            }
          }

          if (require_monotonic && !non_decreasing)
          {
            why = "a monotonic aim graph requires non-decreasing outputs";
            return nullopt;
          }

          aim_graph g;
          for (const knot& k: ks)
            g.knots_[g.knot_count_++] = k;
          g.monotonic_ = non_decreasing;

          return g;
        }

        float
        aim_graph::
        evaluate (float input) const noexcept
        {
          // The class invariant guarantees at least two knots with strictly
          // increasing inputs; evaluate relies on it rather than re-validating.
          //
          assert (knot_count_ >= 2);

          const knot& first (knots_[0]);
          const knot& last (knots_[knot_count_ - 1]);

          // Saturate outside the domain to the end knots.
          //
          if (input <= first.input)
            return first.output;
          if (input >= last.input)
            return last.output;

          // Locate the segment [i-1, i] containing input and interpolate linearly.
          // The span is strictly positive by the increasing-inputs invariant, so the
          // slope is well defined.
          //
          for (size_t i (1); i < knot_count_; ++i)
          {
            if (input <= knots_[i].input)
            {
              const knot& a (knots_[i - 1]);
              const knot& b (knots_[i]);

              const float span (b.input - a.input);
              assert (span > 0.0f);

              const float t ((input - a.input) / span);
              return a.output + (b.output - a.output) * t;
            }
          }

          return last.output;  // Unreachable given the saturation guards above.
        }
      }
    }
  }
}
