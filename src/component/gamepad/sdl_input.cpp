#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/gamepad/gamepad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sdl_input
{
	namespace
	{
		struct sdl_pad_state
		{
			bool connected = false;
			std::int16_t left_x = 0;
			std::int16_t left_y = 0;
			std::int16_t right_x = 0;
			std::int16_t right_y = 0;
			std::uint16_t left_trigger = 0;
			std::uint16_t right_trigger = 0;
			bool button_a = false;
			bool button_b = false;
			bool button_x = false;
			bool button_y = false;
			bool left_shoulder = false;
			bool right_shoulder = false;
			bool start = false;
			bool back = false;
			bool left_stick = false;
			bool right_stick = false;
			bool dpad_up = false;
			bool dpad_down = false;
			bool dpad_left = false;
			bool dpad_right = false;
		};

		[[maybe_unused]] gamepad::backend_state pad{};

		float normalize_axis(const std::int16_t value, const float deadzone)
		{
			if (value == 0)
			{
				return 0.0f;
			}

			const auto normalized = std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
			const auto magnitude = std::fabs(normalized);
			if (magnitude <= deadzone)
			{
				return 0.0f;
			}

			const auto scaled = (magnitude - deadzone) / std::max(0.001f, 1.0f - deadzone);
			return std::copysign(std::clamp(scaled, 0.0f, 1.0f), normalized);
		}

		float normalize_trigger(const std::uint16_t value, const float deadzone)
		{
			const auto normalized = std::clamp(static_cast<float>(value) / 32767.0f, 0.0f, 1.0f);
			if (normalized <= deadzone)
			{
				return 0.0f;
			}

			return std::clamp((normalized - deadzone) / std::max(0.001f, 1.0f - deadzone), 0.0f, 1.0f);
		}

		std::uint32_t build_button_mask(const sdl_pad_state& state)
		{
			std::uint32_t buttons = 0;
			if (state.button_a) buttons |= gamepad::button_a;
			if (state.button_b) buttons |= gamepad::button_b;
			if (state.button_x) buttons |= gamepad::button_x;
			if (state.button_y) buttons |= gamepad::button_y;
			if (state.left_shoulder) buttons |= gamepad::button_left_shoulder;
			if (state.right_shoulder) buttons |= gamepad::button_right_shoulder;
			if (state.start) buttons |= gamepad::button_start;
			if (state.back) buttons |= gamepad::button_back;
			if (state.left_stick) buttons |= gamepad::button_left_thumb;
			if (state.right_stick) buttons |= gamepad::button_right_thumb;
			if (state.dpad_up) buttons |= gamepad::button_dpad_up;
			if (state.dpad_down) buttons |= gamepad::button_dpad_down;
			if (state.dpad_left) buttons |= gamepad::button_dpad_left;
			if (state.dpad_right) buttons |= gamepad::button_dpad_right;
			return buttons;
		}

		[[maybe_unused]] gamepad::normalized_state to_normalized_state(const sdl_pad_state& state)
		{
			constexpr auto stick_deadzone = 0.2f;
			constexpr auto trigger_deadzone = 0.13f;

			gamepad::normalized_state normalized{};
			normalized.buttons = build_button_mask(state);
			normalized.left_stick_x = normalize_axis(state.left_x, stick_deadzone);
			normalized.left_stick_y = normalize_axis(state.left_y, stick_deadzone);
			normalized.right_stick_x = normalize_axis(state.right_x, stick_deadzone);
			normalized.right_stick_y = normalize_axis(state.right_y, stick_deadzone);
			normalized.left_trigger = normalize_trigger(state.left_trigger, trigger_deadzone);
			normalized.right_trigger = normalize_trigger(state.right_trigger, trigger_deadzone);
			return normalized;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// SDL is not linked yet. This file exists to give us a backend-neutral
			// state shape so DualShock/DualSense support can slot in cleanly next.
		}
	};
}

REGISTER_COMPONENT(sdl_input::component)
