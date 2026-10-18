#pragma once

namespace mouse_input
{
	// Mirror QoS PC CL_MouseEvent's absolute UI branch, retaining scoreboard
	// (menu 7) and cl_bypassMouseInput as relative/gameplay input.
	constexpr bool free_menu_cursor(bool foreground, bool windowed, int key_catchers,
		int active_menu, bool bypass_mouse)
	{
		return foreground && windowed && (key_catchers & 0x10) != 0
			&& active_menu != 7 && !bypass_mouse;
	}
}
