#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/sample/frame.hpp>

#include <cassert>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Bounded single-producer / single-consumer ring of input frames.
      //
      // One ring belongs to one device stream. The acquisition path (a Raw Input or
      // HID delivery, or the XInput poll) is the sole producer; the engine frame is
      // the sole consumer. Storage is a fixed inline array, so the per-frame path
      // performs no allocation, which is the invariant the whole sample layer
      // exists to protect.
      //
      // On overflow the newest frame is dropped rather than overwriting an
      // unconsumed one, and the drop is counted so it is observable (the debug ring
      // occupancy widget surfaces it). Capacity must therefore be sized larger than
      // the most frames that can arrive between two consumer drains; with the
      // consumer draining fully each engine frame, a device polling at a few
      // hundred hertz needs only a small ring.
      //
      // The indices are free-running counters; the slot is the counter masked to
      // the capacity, so the empty and full states are distinguished without
      // sacrificing a slot. Capacity must be a power of two for the mask to be
      // exact.
      //
      template <size_t Capacity>
      class frame_ring
      {
      public:
        static constexpr size_t capacity {Capacity};

        static_assert (Capacity >= 2,
                       "a ring needs at least two slots to distinguish states");
        static_assert ((Capacity & (Capacity - 1)) == 0,
                       "ring capacity must be a power of two");

        // Producer: enqueue a frame. Returns false and counts a drop when full.
        //
        bool
        push (const input_frame& f) noexcept
        {
          // The producer owns head and only reads the consumer's published tail.
          //
          const size_t h (head_.load (memory_order_relaxed));
          const size_t t (tail_.load (memory_order_acquire));

          if (h - t >= Capacity)
          {
            dropped_.fetch_add (1, memory_order_relaxed);
            return false;
          }

          // Sequence numbers are monotonic per device stream. This holds after the
          // driver assigns them, so we assert it here rather than validating; a
          // regression that publishes an out-of-order frame is a producer bug, not
          // external data to be tolerated.
          //
          assert (f.sequence > last_sequence_ || h == 0);
          last_sequence_ = f.sequence;

          buf_[h & mask] = f;
          head_.store (h + 1, memory_order_release);
          return true;
        }

        // Consumer: dequeue the oldest frame. Returns false when empty.
        //
        bool
        try_pop (input_frame& out) noexcept
        {
          // The consumer owns tail and only reads the producer's published head.
          //
          const size_t t (tail_.load (memory_order_relaxed));
          const size_t h (head_.load (memory_order_acquire));

          if (t == h)
            return false;

          out = buf_[t & mask];
          tail_.store (t + 1, memory_order_release);
          return true;
        }

        // Frames currently queued. Exact when read by the producer or the consumer;
        // an outside observer may see a value that is already stale.
        //
        size_t
        occupancy () const noexcept
        {
          return head_.load (memory_order_acquire) -
                 tail_.load (memory_order_acquire);
        }

        // Total frames dropped to overflow over the ring's lifetime.
        //
        uint64_t
        dropped () const noexcept
        {
          return dropped_.load (memory_order_relaxed);
        }

      private:
        static constexpr size_t mask {Capacity - 1};

        array<input_frame, Capacity> buf_ {};

        atomic<size_t>   head_ {0};   // Next write counter (producer).
        atomic<size_t>   tail_ {0};   // Next read counter (consumer).
        atomic<uint64_t> dropped_ {0};

        // Producer-only bookkeeping for the monotonic-sequence invariant.
        //
        uint64_t last_sequence_ {0};
      };
    }
  }
}
