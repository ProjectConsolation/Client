#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        // Delta-compressed analog movement for the network usercmd.
        //
        // The base engine transmits movement as bytes, but analog stick movement needs
        // the full signed 8-bit range preserved across the wire. When the forward and
        // right movement differ from the delta baseline, both are packed into sixteen
        // bits XORed with the message key Ã¢â‚¬â€ the engine's per-field obfuscation Ã¢â‚¬â€ behind
        // a single presence bit; when they match the baseline only the clear presence
        // bit is sent.
        //
        // The invariant this packing protects is exact round-trip of the signed
        // forward/right pair: unpacking a packed value with the same key reproduces
        // the original bytes exactly, independent of the key's high bits. This header
        // is the pure codec, free of engine ABI, so the round-trip is unit tested. The
        // message-layer wiring (the MSG_WriteDeltaUsercmdKey / MSG_ReadDeltaUsercmdKey
        // detours) lives with the engine hooks and calls into this codec; it depends
        // on usercmd_s and the MSG_* bit primitives, which the engine ABI header must
        // expose before it can be built.
        //
        struct move_delta
        {
          int8_t forward {0};
          int8_t right {0};

          friend constexpr bool
          operator== (move_delta, move_delta) noexcept = default;
        };

        // Whether the movement differs from the baseline and must be transmitted.
        //
        bool
        move_changed (move_delta from, move_delta to) noexcept;

        // Pack the forward/right pair into the sixteen-bit field XORed with key.
        //
        uint16_t
        pack_move (move_delta to, int key) noexcept;

        // Recover the forward/right pair from a sixteen-bit field XORed with key.
        //
        move_delta
        unpack_move (uint16_t packed, int key) noexcept;
      }
    }
  }
}
