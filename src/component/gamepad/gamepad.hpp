#pragma once

namespace gamepad
{
	bool is_controller_active();
	bool should_hide_cursor();
	void note_mouse_activity();
  void note_key_activity(int key, bool down);
}
