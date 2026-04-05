#pragma once

namespace gamepad
{
	bool should_override_mouse();
	bool consume_right_stick_delta(int& dx, int& dy);
}
