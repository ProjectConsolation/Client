#pragma once

#include <string_view>

namespace game_console
{
	bool is_active();
	void toggle();
	void append_output(std::string_view text);
}
