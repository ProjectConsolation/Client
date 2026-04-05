#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "scheduler.hpp"
#include "gamepad.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <algorithm>
#include <array>
#include <Xinput.h>

#pragma comment(lib, "xinput9_1_0.lib")

namespace gamepad
{
	namespace
	{
		struct controller_state
		{
			bool connected = false;
			XINPUT_STATE state{};
			XINPUT_STATE previous_state{};
			std::array<bool, 4> menu_direction_down{};
			std::array<DWORD, 4> menu_next_repeat_time{};
			std::array<bool, 4> move_direction_down{};
			DWORD last_activity_time = 0;
			float accumulated_look_x = 0.0f;
			float accumulated_look_y = 0.0f;
		};

		controller_state pad{};
		bool shutdown_requested = false;

		enum direction
		{
			dir_up = 0,
			dir_down,
			dir_left,
			dir_right,
		};

		struct digital_mapping
		{
			WORD xinput_mask;
			int gameplay_key;
			int menu_key;
		};

		constexpr digital_mapping digital_buttons[]
		{
			{XINPUT_GAMEPAD_A, ' ', game::K_ENTER},
			{XINPUT_GAMEPAD_B, 'c', game::K_ESCAPE},
			{XINPUT_GAMEPAD_X, 'f', 'r'},
			{XINPUT_GAMEPAD_Y, 'q', 'q'},
			{XINPUT_GAMEPAD_LEFT_SHOULDER, 'q', game::K_PGUP},
			{XINPUT_GAMEPAD_RIGHT_SHOULDER, 'e', game::K_PGDN},
			{XINPUT_GAMEPAD_START, game::K_ESCAPE, game::K_ENTER},
			{XINPUT_GAMEPAD_BACK, game::K_TAB, game::K_ESCAPE},
			{XINPUT_GAMEPAD_LEFT_THUMB, game::K_SHIFT, 0},
			{XINPUT_GAMEPAD_RIGHT_THUMB, 'v', 0},
			{XINPUT_GAMEPAD_DPAD_UP, game::K_UPARROW, game::K_UPARROW},
			{XINPUT_GAMEPAD_DPAD_DOWN, game::K_DOWNARROW, game::K_DOWNARROW},
			{XINPUT_GAMEPAD_DPAD_LEFT, game::K_LEFTARROW, game::K_LEFTARROW},
			{XINPUT_GAMEPAD_DPAD_RIGHT, game::K_RIGHTARROW, game::K_RIGHTARROW},
		};

		bool is_gamepad_enabled()
		{
			return dvars::gpad_enabled && dvars::gpad_enabled->current.enabled;
		}

		bool is_menu_mode()
		{
			return game::keyCatchers && *game::keyCatchers != 0;
		}

		int get_deadzone()
		{
			if (!dvars::gpad_stick_deadzone_min)
			{
				return XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
			}

			const auto value = std::clamp(dvars::gpad_stick_deadzone_min->current.value, 0.0f, 1.0f);
			return static_cast<int>(value * 32767.0f);
		}

		DWORD get_first_repeat_delay()
		{
			return dvars::gpad_menu_scroll_delay_first
				? static_cast<DWORD>(std::max(0, dvars::gpad_menu_scroll_delay_first->current.integer))
				: 420u;
		}

		DWORD get_rest_repeat_delay()
		{
			return dvars::gpad_menu_scroll_delay_rest
				? static_cast<DWORD>(std::max(0, dvars::gpad_menu_scroll_delay_rest->current.integer))
				: 210u;
		}

		void set_bool_dvar(game::dvar_s* dvar, const bool value)
		{
			if (!dvar)
			{
				return;
			}

			dvar->current.enabled = value;
			dvar->latched.enabled = value;
			dvar->reset.enabled = value;
		}

		void fire_key_event(const int key, const bool down, const DWORD time)
		{
			if (!key)
			{
				return;
			}

			game::CL_KeyEvent(0, key, down ? 1 : 0, time);
		}

		void pulse_key(const int key, const DWORD time)
		{
			fire_key_event(key, true, time);
			fire_key_event(key, false, time);
		}

		void debug_print(const char* fmt, ...)
		{
			if (!dvars::gpad_debug || !dvars::gpad_debug->current.enabled)
			{
				return;
			}

			char buffer[512]{};
			va_list ap{};
			va_start(ap, fmt);
			vsnprintf_s(buffer, _TRUNCATE, fmt, ap);
			va_end(ap);

			game::Com_Printf(0, "%s", buffer);
		}

		void release_all_inputs(const DWORD time)
		{
			for (const auto& mapping : digital_buttons)
			{
				fire_key_event(mapping.gameplay_key, false, time);
				fire_key_event(mapping.menu_key, false, time);
			}

			fire_key_event(game::K_MOUSE1, false, time);
			fire_key_event(game::K_MOUSE2, false, time);
			fire_key_event('w', false, time);
			fire_key_event('a', false, time);
			fire_key_event('s', false, time);
			fire_key_event('d', false, time);
		}

		void update_activity(const DWORD time)
		{
			pad.last_activity_time = time;
		}

		void handle_digital_buttons(const XINPUT_GAMEPAD& current, const XINPUT_GAMEPAD& previous, const DWORD time)
		{
			const auto menu_mode = is_menu_mode();

			for (const auto& mapping : digital_buttons)
			{
				const auto was_down = (previous.wButtons & mapping.xinput_mask) != 0;
				const auto is_down = (current.wButtons & mapping.xinput_mask) != 0;

				if (was_down == is_down)
				{
					continue;
				}

				const auto key = menu_mode ? mapping.menu_key : mapping.gameplay_key;
				fire_key_event(key, is_down, time);
				if (is_down)
				{
					update_activity(time);
				}
			}
		}

		void handle_triggers(const BYTE current_value, const BYTE previous_value, const int key, const DWORD time)
		{
			constexpr BYTE threshold = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
			const auto was_down = previous_value > threshold;
			const auto is_down = current_value > threshold;

			if (was_down == is_down)
			{
				return;
			}

			fire_key_event(key, is_down, time);
			if (is_down)
			{
				update_activity(time);
			}
		}

		void handle_menu_direction(const direction dir, const int key, const bool active, const DWORD time)
		{
			auto& is_down = pad.menu_direction_down[dir];
			auto& next_repeat = pad.menu_next_repeat_time[dir];

			if (active)
			{
				if (!is_down)
				{
					pulse_key(key, time);
					is_down = true;
					next_repeat = time + get_first_repeat_delay();
					update_activity(time);
					return;
				}

				if (time >= next_repeat)
				{
					pulse_key(key, time);
					next_repeat = time + get_rest_repeat_delay();
					update_activity(time);
				}

				return;
			}

			is_down = false;
			next_repeat = 0;
		}

		void handle_left_stick_menu_input(const XINPUT_GAMEPAD& state, const DWORD time)
		{
			const auto deadzone = get_deadzone();
			handle_menu_direction(dir_up, game::K_UPARROW, state.sThumbLY >= deadzone, time);
			handle_menu_direction(dir_down, game::K_DOWNARROW, state.sThumbLY <= -deadzone, time);
			handle_menu_direction(dir_left, game::K_LEFTARROW, state.sThumbLX <= -deadzone, time);
			handle_menu_direction(dir_right, game::K_RIGHTARROW, state.sThumbLX >= deadzone, time);
		}

		void set_move_key(const direction dir, const int key, const bool active, const DWORD time)
		{
			auto& was_down = pad.move_direction_down[dir];
			if (was_down == active)
			{
				return;
			}

			fire_key_event(key, active, time);
			was_down = active;
			if (active)
			{
				update_activity(time);
			}
		}

		void handle_left_stick_game_input(const XINPUT_GAMEPAD& state, const DWORD time)
		{
			const auto deadzone = get_deadzone();
			set_move_key(dir_up, 'w', state.sThumbLY >= deadzone, time);
			set_move_key(dir_down, 's', state.sThumbLY <= -deadzone, time);
			set_move_key(dir_left, 'a', state.sThumbLX <= -deadzone, time);
			set_move_key(dir_right, 'd', state.sThumbLX >= deadzone, time);
		}

		void accumulate_right_stick_look(const XINPUT_GAMEPAD& state, const DWORD time)
		{
			const auto deadzone = get_deadzone();
			const auto normalize_axis = [deadzone](const SHORT value)
			{
				if (std::abs(static_cast<int>(value)) <= deadzone)
				{
					return 0.0f;
				}

				const auto sign = value < 0 ? -1.0f : 1.0f;
				const auto magnitude = (std::abs(static_cast<int>(value)) - deadzone) / static_cast<float>(32767 - deadzone);
				return std::clamp(sign * magnitude, -1.0f, 1.0f);
			};

			const auto x = normalize_axis(state.sThumbRX);
			const auto y = normalize_axis(state.sThumbRY);

			if (x == 0.0f && y == 0.0f)
			{
				return;
			}

			constexpr float look_scale = 14.0f;
			pad.accumulated_look_x += x * look_scale;
			pad.accumulated_look_y += y * look_scale;
			update_activity(time);
		}

		void poll_controller()
		{
			if (shutdown_requested)
			{
				return;
			}

			XINPUT_STATE state{};
			const auto result = XInputGetState(0, &state);
			const auto connected = result == ERROR_SUCCESS;
			const auto time = GetTickCount();

			set_bool_dvar(dvars::gpad_present, connected);

			if (!connected)
			{
				if (pad.connected)
				{
					release_all_inputs(time);
				}

				pad = {};
				set_bool_dvar(dvars::gpad_in_use, false);
				return;
			}

			pad.connected = true;
			pad.state = state;

			if (!is_gamepad_enabled())
			{
				release_all_inputs(time);
				pad.previous_state = pad.state;
				set_bool_dvar(dvars::gpad_in_use, false);
				return;
			}

			handle_digital_buttons(pad.state.Gamepad, pad.previous_state.Gamepad, time);

			if (is_menu_mode())
			{
				handle_left_stick_menu_input(pad.state.Gamepad, time);
			}
			else
			{
				handle_left_stick_game_input(pad.state.Gamepad, time);
			}

			handle_triggers(pad.state.Gamepad.bLeftTrigger, pad.previous_state.Gamepad.bLeftTrigger, game::K_MOUSE2, time);
			handle_triggers(pad.state.Gamepad.bRightTrigger, pad.previous_state.Gamepad.bRightTrigger, game::K_MOUSE1, time);
			accumulate_right_stick_look(pad.state.Gamepad, time);

			const auto in_use = (time - pad.last_activity_time) <= 2000u;
			set_bool_dvar(dvars::gpad_in_use, in_use);

			debug_print("gpad: buttons=0x%04X lx=%d ly=%d rx=%d ry=%d lt=%u rt=%u menu=%d in_use=%d\n",
				pad.state.Gamepad.wButtons,
				pad.state.Gamepad.sThumbLX,
				pad.state.Gamepad.sThumbLY,
				pad.state.Gamepad.sThumbRX,
				pad.state.Gamepad.sThumbRY,
				pad.state.Gamepad.bLeftTrigger,
				pad.state.Gamepad.bRightTrigger,
				static_cast<int>(is_menu_mode()),
				static_cast<int>(in_use));

			pad.previous_state = pad.state;
		}
	}

	bool should_override_mouse()
	{
		return is_gamepad_enabled() && dvars::gpad_in_use && dvars::gpad_in_use->current.enabled;
	}

	bool consume_right_stick_delta(int& dx, int& dy)
	{
		dx = static_cast<int>(pad.accumulated_look_x);
		dy = static_cast<int>(-pad.accumulated_look_y);

		pad.accumulated_look_x -= static_cast<float>(dx);
		pad.accumulated_look_y += static_cast<float>(dy);

		return dx != 0 || dy != 0;
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::loop([]()
			{
				poll_controller();
			}, scheduler::main, 16ms);

			scheduler::on_shutdown([]()
			{
				shutdown_requested = true;
				release_all_inputs(GetTickCount());
				set_bool_dvar(dvars::gpad_present, false);
				set_bool_dvar(dvars::gpad_in_use, false);
			});
		}

		void pre_destroy() override
		{
			shutdown_requested = true;
		}
	};
}

REGISTER_COMPONENT(gamepad::component)
