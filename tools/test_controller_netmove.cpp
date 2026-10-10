#include "../src/component/gamepad/controller/engine/netmove.hpp"
#include <cassert>
#include <array>
int main()
{
  using namespace gamepad::unstable::controller::engine;
  const std::array<int, 6> keys {0, 1, -1, 0x12345678, 0x7fffffff, -2147483647};
  for (int forward = -128; forward <= 127; ++forward)
    for (int right = -128; right <= 127; ++right)
      for (int key : keys)
      {
        const move_delta to {static_cast<int8_t>(forward), static_cast<int8_t>(right)};
        assert(unpack_move(pack_move(to, key), key) == to);
        assert(!move_changed(to, to));
        const move_delta changed {static_cast<int8_t>(forward == 127 ? -128 : forward + 1), to.right};
        assert(move_changed(to, changed));
      }
  // Same direction, different speed MUST change the command/presence bits.
  assert(move_changed({11, 0}, {12, 0}));
  assert(move_changed({0, -12}, {0, -11}));
}
