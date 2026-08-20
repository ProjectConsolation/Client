#include <std_include.hpp>

#include <component/gamepad/controller/engine/netmove.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        bool
        move_changed (move_delta from, move_delta to) noexcept
        {
          return to.forward != from.forward || to.right != from.right;
        }

        uint16_t
        pack_move (move_delta to, int key) noexcept
        {
          // Assemble the signed bytes into a 16-bit plain field (forward low, right
          // high), then XOR with the key. Casting through uint8_t first keeps the sign
          // bits in place rather than sign-extending into the upper byte.
          //
          const int plain (static_cast<uint8_t> (to.forward) |
                           (static_cast<uint8_t> (to.right) << 8));

          return static_cast<uint16_t> ((key ^ plain) & 0xFFFF);
        }

        move_delta
        unpack_move (uint16_t packed, int key) noexcept
        {
          // XOR removes the key (its high bits fall away with the 16-bit mask), and
          // the two bytes are reinterpreted as signed movement.
          //
          const int bits ((key ^ packed) & 0xFFFF);

          return {static_cast<int8_t> (bits & 0xFF),
                  static_cast<int8_t> ((bits >> 8) & 0xFF)};
        }
      }
    }
  }
}
