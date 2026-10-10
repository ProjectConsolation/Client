#include "../src/component/gamepad/controller/engine/stance.hpp"
#include <cassert>
int main()
{
  using namespace gamepad::unstable::controller::engine;
  assert(!stance_hold_elapsed(1299, 1000, 300));
  assert(stance_hold_elapsed(1300, 1000, 300));
  assert(stance_hold_elapsed(0x20, 0xfffffff0, 48));
  assert(!stance_hold_elapsed(0x1f, 0xfffffff0, 48));
  assert(held_stance(0) == 2);
  assert(held_stance(1) == 2);
  assert(held_stance(2) == 0);
}
