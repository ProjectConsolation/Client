#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "scheduler.hpp"
#include "gamepad.hpp"
#include "game_console.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <Xinput.h>

#include <utils/hook.hpp>

#pragma comment(lib, "xinput9_1_0.lib")

namespace gamepad
{
	namespace
	{
		struct controller_state
		{
			bool connected = false;
			bool menu_mode = false;
			XINPUT_STATE state{};
			XINPUT_STATE previous_state{};
			std::array<bool, 4> menu_direction_down{};
			std::array<DWORD, 4> menu_next_repeat_time{};
			DWORD last_activity_time = 0;
			float left_stick_x = 0.0f;
			float left_stick_y = 0.0f;
			float right_stick_x = 0.0f;
			float right_stick_y = 0.0f;
			float left_trigger = 0.0f;
			float right_trigger = 0.0f;
			float yaw_remainder = 0.0f;
			float pitch_remainder = 0.0f;
		};

		controller_state pad{};
		bool shutdown_requested = false;
		bool create_cmd_callsite_patched = false;
		std::array<std::uint8_t, 5> original_create_cmd_call{};

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
			bool handled_natively_in_gameplay;
		};

		constexpr digital_mapping digital_buttons[]
		{
			{XINPUT_GAMEPAD_A, 0, game::K_ENTER, true},
			{XINPUT_GAMEPAD_B, 0, game::K_ESCAPE, true},
			{XINPUT_GAMEPAD_X, 0, 0, true},
			{XINPUT_GAMEPAD_Y, game::K_BUTTON_Y, 0, false},
			{XINPUT_GAMEPAD_LEFT_SHOULDER, game::K_BUTTON_LSHLDR, game::K_PGUP, false},
			{XINPUT_GAMEPAD_RIGHT_SHOULDER, game::K_BUTTON_RSHLDR, game::K_PGDN, false},
			{XINPUT_GAMEPAD_START, game::K_BUTTON_START, game::K_ENTER, false},
			{XINPUT_GAMEPAD_BACK, game::K_BUTTON_BACK, game::K_ESCAPE, false},
			{XINPUT_GAMEPAD_LEFT_THUMB, 0, 0, true},
			{XINPUT_GAMEPAD_RIGHT_THUMB, 0, 0, true},
			{XINPUT_GAMEPAD_DPAD_UP, game::K_DPAD_UP, game::K_UPARROW, false},
			{XINPUT_GAMEPAD_DPAD_DOWN, game::K_DPAD_DOWN, game::K_DOWNARROW, false},
			{XINPUT_GAMEPAD_DPAD_LEFT, game::K_DPAD_LEFT, game::K_LEFTARROW, false},
			{XINPUT_GAMEPAD_DPAD_RIGHT, game::K_DPAD_RIGHT, game::K_RIGHTARROW, false},
		};

		bool is_gamepad_enabled()
		{
			return dvars::gpad_enabled && dvars::gpad_enabled->current.enabled;
		}

		bool is_menu_mode()
		{
			return game_console::is_active() || (game::keyCatchers && *game::keyCatchers != 0);
		}

		bool should_drive_native_cmd()
		{
			return is_gamepad_enabled()
				&& pad.connected
				&& !shutdown_requested
				&& !is_menu_mode();
		}

		float get_stick_deadzone_min()
		{
			if (!dvars::gpad_stick_deadzone_min)
			{
				return 0.2f;
			}

			return std::clamp(dvars::gpad_stick_deadzone_min->current.value, 0.0f, 1.0f);
		}

		float get_stick_deadzone_max()
		{
			if (!dvars::gpad_stick_deadzone_max)
			{
				return 0.01f;
			}

			return std::clamp(dvars::gpad_stick_deadzone_max->current.value, 0.0f, 1.0f);
		}

		float get_trigger_deadzone()
		{
			if (!dvars::gpad_button_deadzone)
			{
				return 0.13f;
			}

			return std::clamp(dvars::gpad_button_deadzone->current.value, 0.0f, 1.0f);
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

		void normalize_stick_pair(const SHORT x, const SHORT y, float& out_x, float& out_y)
		{
			if (x == 0 && y == 0)
			{
				out_x = 0.0f;
				out_y = 0.0f;
				return;
			}

			float stick_x = static_cast<float>(x) / static_cast<float>(std::numeric_limits<SHORT>::max());
			float stick_y = static_cast<float>(y) / static_cast<float>(std::numeric_limits<SHORT>::max());
			const auto length = std::sqrt((stick_x * stick_x) + (stick_y * stick_y));

			if (length <= 0.0f)
			{
				out_x = 0.0f;
				out_y = 0.0f;
				return;
			}

			const auto min_deadzone = get_stick_deadzone_min();
			const auto max_deadzone = get_stick_deadzone_max();
			const auto deadzone_total = min_deadzone + max_deadzone;

			float scaled_length = 0.0f;
			if (length >= min_deadzone)
			{
				if (length <= (1.0f - max_deadzone))
				{
					scaled_length = (length - min_deadzone) / std::max(0.001f, 1.0f - deadzone_total);
				}
				else
				{
					scaled_length = 1.0f;
				}
			}

			const auto normalized_x = stick_x / length;
			const auto normalized_y = stick_y / length;
			out_x = normalized_x * scaled_length;
			out_y = normalized_y * scaled_length;
		}

		float normalize_trigger(const BYTE value)
		{
			const auto normalized = static_cast<float>(value) / 255.0f;
			const auto deadzone = get_trigger_deadzone();
			if (normalized <= deadzone)
			{
				return 0.0f;
			}

			return std::clamp((normalized - deadzone) / std::max(0.001f, 1.0f - deadzone), 0.0f, 1.0f);
		}

		void release_all_inputs(const DWORD time)
		{
			for (const auto& mapping : digital_buttons)
			{
				fire_key_event(mapping.gameplay_key, false, time);
				fire_key_event(mapping.menu_key, false, time);
			}

			pad.menu_direction_down.fill(false);
			pad.menu_next_repeat_time.fill(0);
			pad.yaw_remainder = 0.0f;
			pad.pitch_remainder = 0.0f;
		}

		void update_activity(const DWORD time)
		{
			pad.last_activity_time = time;
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

		void handle_left_stick_menu_input(const DWORD time)
		{
			const auto threshold = std::max(0.05f, get_stick_deadzone_min() + 0.05f);
			handle_menu_direction(dir_up, game::K_UPARROW, pad.left_stick_y >= threshold, time);
			handle_menu_direction(dir_down, game::K_DOWNARROW, pad.left_stick_y <= -threshold, time);
			handle_menu_direction(dir_left, game::K_LEFTARROW, pad.left_stick_x <= -threshold, time);
			handle_menu_direction(dir_right, game::K_RIGHTARROW, pad.left_stick_x >= threshold, time);
		}

		void update_analog_state(const XINPUT_GAMEPAD& state)
		{
			normalize_stick_pair(state.sThumbLX, state.sThumbLY, pad.left_stick_x, pad.left_stick_y);
			normalize_stick_pair(state.sThumbRX, state.sThumbRY, pad.right_stick_x, pad.right_stick_y);
			pad.left_trigger = normalize_trigger(state.bLeftTrigger);
			pad.right_trigger = normalize_trigger(state.bRightTrigger);
		}

		void handle_digital_buttons(const XINPUT_GAMEPAD& current, const XINPUT_GAMEPAD& previous, const DWORD time)
		{
			for (const auto& mapping : digital_buttons)
			{
				const auto was_down = (previous.wButtons & mapping.xinput_mask) != 0;
				const auto is_down = (current.wButtons & mapping.xinput_mask) != 0;

				if (was_down == is_down)
				{
					continue;
				}

				if (pad.menu_mode)
				{
					fire_key_event(mapping.menu_key, is_down, time);
				}
				else if (!mapping.handled_natively_in_gameplay)
				{
					fire_key_event(mapping.gameplay_key, is_down, time);
				}

				if (is_down)
				{
					update_activity(time);
				}
			}
		}

		int normalize_angle_units(int angle)
		{
			while (angle > 32768)
			{
				angle -= 65536;
			}

			while (angle < -32768)
			{
				angle += 65536;
			}

			return angle;
		}

		std::int8_t clamp_cmd_axis(const int value)
		{
			return static_cast<std::int8_t>(std::clamp(value, -127, 127));
		}

		float get_view_sensitivity()
		{
			const auto* const dvar = game::Dvar_FindVar("input_viewSensitivity");
			return dvar ? std::clamp(dvar->current.value, 0.01f, 30.0f) : 1.0f;
		}

		void apply_native_gamepad_to_cmd(game::usercmd_t* cmd)
		{
			if (!cmd || !should_drive_native_cmd())
			{
				return;
			}

			const auto forward = pad.left_stick_y;
			const auto side = pad.left_stick_x;
			const auto yaw_axis = -pad.right_stick_x;
			auto pitch_axis = pad.right_stick_y;

			if (!dvars::input_invertPitch || !dvars::input_invertPitch->current.enabled)
			{
				pitch_axis *= -1.0f;
			}

			float move_scale = static_cast<float>(std::numeric_limits<std::int8_t>::max());
			if (std::fabs(side) > 0.0f || std::fabs(forward) > 0.0f)
			{
				const auto length = std::fabs(side) <= std::fabs(forward)
					? (forward != 0.0f ? side / forward : 0.0f)
					: (side != 0.0f ? forward / side : 0.0f);
				move_scale = std::sqrt((length * length) + 1.0f) * move_scale;
			}

			cmd->forwardmove = clamp_cmd_axis(static_cast<int>(cmd->forwardmove) + static_cast<int>(std::floor(forward * move_scale)));
			cmd->rightmove = clamp_cmd_axis(static_cast<int>(cmd->rightmove) + static_cast<int>(std::floor(side * move_scale)));

			const auto turn_rate = 1200.0f * get_view_sensitivity();
			pad.yaw_remainder += yaw_axis * turn_rate;
			pad.pitch_remainder += pitch_axis * turn_rate;

			const auto yaw_delta = static_cast<int>(pad.yaw_remainder);
			const auto pitch_delta = static_cast<int>(pad.pitch_remainder);

			pad.yaw_remainder -= static_cast<float>(yaw_delta);
			pad.pitch_remainder -= static_cast<float>(pitch_delta);

			cmd->angles[1] = normalize_angle_units(cmd->angles[1] + yaw_delta);
			cmd->angles[0] = normalize_angle_units(cmd->angles[0] + pitch_delta);

			if (pad.right_trigger > 0.0f)
			{
				cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons | game::BUTTON_ATTACK);
			}

			if (pad.left_trigger > 0.0f)
			{
				cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons | game::BUTTON_ADS);
			}

			if ((pad.state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0)
			{
				cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons | game::BUTTON_SPRINT);
			}

			if ((pad.state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0)
			{
				cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons | game::BUTTON_MELEE_BREATH);
			}

			if ((pad.state.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0)
			{
				cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons | game::BUTTON_USE | game::BUTTON_RELOAD);
			}

			if ((pad.state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0)
			{
				cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons | game::BUTTON_CROUCH);
			}

			if ((pad.state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0)
			{
				cmd->upmove = 127;
			}
		}

		game::usercmd_t* __cdecl build_cmd_body(game::usercmd_t* cmd, const int local_client_num)
		{
			// This is the stock QoS usercmd builder reached from 0x102FFCD4.
			// We let the game build its normal command first, then layer native pad input on top.
			const auto func_loc = static_cast<int>(game::game_offset(0x102FFB80));
			game::usercmd_t* result = nullptr;

			__asm
			{
				push local_client_num
				mov eax, cmd
				call func_loc
				add esp, 4
				mov result, eax
			}

			apply_native_gamepad_to_cmd(result);
			return result;
		}

		__declspec(naked) void build_cmd_stub()
		{
			__asm
			{
				mov edx, [esp + 4]
				push edx
				push eax
				call build_cmd_body
				add esp, 8
				ret
			}
		}

		void install_native_cmd_hook()
		{
			if (create_cmd_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_create_cmd_call.size(); ++i)
			{
				original_create_cmd_call[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFCD4 + i));
			}

			utils::hook::call(game::game_offset(0x102FFCD4), build_cmd_stub);
			create_cmd_callsite_patched = true;
		}

		void restore_native_cmd_hook()
		{
			if (!create_cmd_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_create_cmd_call.size(); ++i)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFCD4 + static_cast<int>(i)), original_create_cmd_call[i]);
			}

			create_cmd_callsite_patched = false;
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
			update_analog_state(pad.state.Gamepad);

			const auto menu_mode = is_menu_mode();
			if (menu_mode != pad.menu_mode)
			{
				release_all_inputs(time);
				pad.previous_state = {};
				pad.menu_mode = menu_mode;
			}

			if (!is_gamepad_enabled())
			{
				release_all_inputs(time);
				pad.previous_state = pad.state;
				set_bool_dvar(dvars::gpad_in_use, false);
				return;
			}

			handle_digital_buttons(pad.state.Gamepad, pad.previous_state.Gamepad, time);

			if (pad.menu_mode)
			{
				handle_left_stick_menu_input(time);
			}

			const auto has_analog_activity =
				std::fabs(pad.left_stick_x) > 0.0f
				|| std::fabs(pad.left_stick_y) > 0.0f
				|| std::fabs(pad.right_stick_x) > 0.0f
				|| std::fabs(pad.right_stick_y) > 0.0f
				|| pad.left_trigger > 0.0f
				|| pad.right_trigger > 0.0f;

			if (has_analog_activity || pad.state.Gamepad.wButtons != 0)
			{
				update_activity(time);
			}

			const auto in_use = (time - pad.last_activity_time) <= 2000u;
			set_bool_dvar(dvars::gpad_in_use, in_use);

			debug_print("gpad: buttons=0x%04X lx=%.3f ly=%.3f rx=%.3f ry=%.3f lt=%.3f rt=%.3f menu=%d in_use=%d\n",
				pad.state.Gamepad.wButtons,
				pad.left_stick_x,
				pad.left_stick_y,
				pad.right_stick_x,
				pad.right_stick_y,
				pad.left_trigger,
				pad.right_trigger,
				static_cast<int>(pad.menu_mode),
				static_cast<int>(in_use));

			pad.previous_state = pad.state;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			install_native_cmd_hook();

			scheduler::loop([]()
			{
				poll_controller();
			}, scheduler::main, 16ms);

			scheduler::on_shutdown([]()
			{
				shutdown_requested = true;
				release_all_inputs(GetTickCount());
				restore_native_cmd_hook();
				set_bool_dvar(dvars::gpad_present, false);
				set_bool_dvar(dvars::gpad_in_use, false);
			});
		}

		void pre_destroy() override
		{
			shutdown_requested = true;
			restore_native_cmd_hook();
		}
	};
}

REGISTER_COMPONENT(gamepad::component)
