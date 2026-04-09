#pragma once

#include <array>
#include <cstdint>

namespace gamepad
{
	enum direction : std::size_t
	{
		dir_up = 0,
		dir_down,
		dir_left,
		dir_right,
		dir_count,
	};

	enum button_mask : std::uint32_t
	{
		button_a = 1u << 0,
		button_b = 1u << 1,
		button_x = 1u << 2,
		button_y = 1u << 3,
		button_left_shoulder = 1u << 4,
		button_right_shoulder = 1u << 5,
		button_start = 1u << 6,
		button_back = 1u << 7,
		button_left_thumb = 1u << 8,
		button_right_thumb = 1u << 9,
		button_dpad_up = 1u << 10,
		button_dpad_down = 1u << 11,
		button_dpad_left = 1u << 12,
		button_dpad_right = 1u << 13,
	};

	struct normalized_state
	{
		std::uint32_t buttons = 0;
		float left_stick_x = 0.0f;
		float left_stick_y = 0.0f;
		float right_stick_x = 0.0f;
		float right_stick_y = 0.0f;
		float left_trigger = 0.0f;
		float right_trigger = 0.0f;
	};

	struct backend_state
	{
		bool connected = false;
		bool menu_mode = false;
		normalized_state current{};
		normalized_state previous{};
		std::array<bool, dir_count> menu_direction_down{};
		std::array<bool, dir_count> apad_direction_down{};
		std::array<unsigned long, dir_count> menu_next_repeat_time{};
		unsigned long last_activity_time = 0;
	};
}
