#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/utils/scheduler.hpp"
#include "component/engine/patches/patches.hpp"
#include "gamepad.hpp"
#include "component/engine/console/game_console.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <Xinput.h>

#include <utils/hook.hpp>

#pragma comment(lib, "xinput9_1_0.lib")

namespace xinput
{
	bool should_hide_cursor_now();

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
			std::array<DWORD, 4> menu_hold_start_time{};
			DWORD last_activity_time = 0;
			float left_stick_x = 0.0f;
			float left_stick_y = 0.0f;
			float right_stick_x = 0.0f;
			float right_stick_y = 0.0f;
			float left_trigger = 0.0f;
			float right_trigger = 0.0f;
		};

		controller_state pad{};
		bool shutdown_requested = false;
		bool create_cmd_callsite_patched = false;
		bool native_look_callsite_patched = false;
		bool draw_crosshair_callsite_patched = false;
		bool move_axis_callsite_patched = false;
		bool usercmd_movement_patched = false;
		std::array<std::uint8_t, 5> original_create_cmd_call{};
		std::array<std::uint8_t, 5> original_native_look_call{};
		std::array<std::uint8_t, 5> original_draw_crosshair_call{};
		std::array<std::uint8_t, 5> original_move_axis_call_1{};
		std::array<std::uint8_t, 5> original_move_axis_call_2{};
		std::array<std::uint8_t, 5> original_move_axis_call_3{};
		std::array<std::uint8_t, 5> original_move_axis_call_4{};
		std::array<std::uint8_t, 5> original_pack_current_move_jump{};
		std::array<std::uint8_t, 5> original_pack_previous_move_jump{};
		std::array<std::uint8_t, 5> original_unpack_base_move_a_jump{};
		std::array<std::uint8_t, 5> original_unpack_base_move_b_jump{};
		std::array<std::uint8_t, 5> original_unpack_exact_move_a_jump{};
		std::array<std::uint8_t, 5> original_unpack_exact_move_b_jump{};
		std::uint16_t original_write_move_bits_a = 0;
		std::uint16_t original_write_move_bits_b = 0;
		std::uint16_t original_read_move_bits_a = 0;
		std::uint16_t original_read_move_bits_b = 0;
		std::atomic<DWORD> last_mouse_activity_time{ 0 };
		bool cursor_hidden_for_gamepad = false;
		DWORD last_analog_update_time = 0;
		float analog_frame_seconds = 1.0f / 60.0f;
		std::uint32_t pack_current_move_rejoin = 0;
		std::uint32_t pack_previous_move_rejoin = 0;
		std::uint32_t unpack_base_move_a_rejoin = 0;
		std::uint32_t unpack_base_move_b_rejoin = 0;
		std::uint32_t unpack_exact_move_a_rejoin = 0;
		std::uint32_t unpack_exact_move_b_rejoin = 0;

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
			{XINPUT_GAMEPAD_A, game::K_BUTTON_A, game::K_ENTER},
			{XINPUT_GAMEPAD_B, game::K_BUTTON_B, game::K_ESCAPE},
			{XINPUT_GAMEPAD_X, game::K_BUTTON_X, 0},
			{XINPUT_GAMEPAD_Y, game::K_BUTTON_Y, 0},
			{XINPUT_GAMEPAD_LEFT_SHOULDER, game::K_BUTTON_LSHLDR, game::K_PGUP},
			{XINPUT_GAMEPAD_RIGHT_SHOULDER, game::K_BUTTON_RSHLDR, game::K_PGDN},
			{XINPUT_GAMEPAD_START, game::K_BUTTON_START, game::K_ENTER},
			{XINPUT_GAMEPAD_BACK, game::K_BUTTON_BACK, game::K_ESCAPE},
			{XINPUT_GAMEPAD_LEFT_THUMB, game::K_BUTTON_LSTICK, 0},
			{XINPUT_GAMEPAD_RIGHT_THUMB, game::K_BUTTON_RSTICK, 0},
			{XINPUT_GAMEPAD_DPAD_UP, game::K_DPAD_UP, game::K_UPARROW},
			{XINPUT_GAMEPAD_DPAD_DOWN, game::K_DPAD_DOWN, game::K_DOWNARROW},
			{XINPUT_GAMEPAD_DPAD_LEFT, game::K_DPAD_LEFT, game::K_LEFTARROW},
			{XINPUT_GAMEPAD_DPAD_RIGHT, game::K_DPAD_RIGHT, game::K_RIGHTARROW},
		};

		bool is_dpad_mapping(const WORD mask)
		{
			return mask == XINPUT_GAMEPAD_DPAD_UP
				|| mask == XINPUT_GAMEPAD_DPAD_DOWN
				|| mask == XINPUT_GAMEPAD_DPAD_LEFT
				|| mask == XINPUT_GAMEPAD_DPAD_RIGHT;
		}

		bool is_gamepad_enabled()
		{
			return dvars::gpad_enabled && dvars::gpad_enabled->current.enabled;
		}

		bool is_gamepad_in_use()
		{
			return dvars::gpad_in_use && dvars::gpad_in_use->current.enabled;
		}

		bool is_menu_mode()
		{
			const auto* const cl_ingame = game::Dvar_FindVar("cl_ingame");
			const auto in_game = cl_ingame && cl_ingame->current.enabled;
			return game_console::is_active()
				|| !in_game
				|| (game::keyCatchers && *game::keyCatchers != 0);
		}

		bool should_drive_native_cmd()
		{
			return is_gamepad_enabled()
				&& pad.connected
				&& is_gamepad_in_use()
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

		DWORD get_min_repeat_delay()
		{
			return dvars::gpad_menu_scroll_delay_min
				? static_cast<DWORD>(std::max(0, dvars::gpad_menu_scroll_delay_min->current.integer))
				: 50u;
		}

		DWORD get_repeat_accel_time()
		{
			return dvars::gpad_menu_scroll_accel_time
				? static_cast<DWORD>(std::max(0, dvars::gpad_menu_scroll_accel_time->current.integer))
				: 1500u;
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

		void pulse_key_event(const int key, const DWORD time)
		{
			fire_key_event(key, true, time);
			fire_key_event(key, false, time);
		}

		const char* get_fallback_gameplay_command(const int key)
		{
			switch (key)
			{
			case game::K_BUTTON_A:
				return "+gostand";
			case game::K_BUTTON_B:
				return "+stance";
			case game::K_BUTTON_X:
				return "+usereload";
			case game::K_BUTTON_Y:
				return "weapnext";
			case game::K_BUTTON_LTRIG:
				return "+speed";
			case game::K_BUTTON_LSHLDR:
				return "+smoke";
			case game::K_BUTTON_RSHLDR:
				return "+frag";
			case game::K_BUTTON_START:
				return "togglemenu";
			case game::K_BUTTON_BACK:
				return "+scores";
			case game::K_BUTTON_LSTICK:
				return "+breath_sprint";
			case game::K_BUTTON_RSTICK:
				return "+melee";
			case game::K_BUTTON_RTRIG:
				return "+attack";
			default:
				return nullptr;
			}
		}

		void queue_command(const int key, const char* command, const bool down, const DWORD time)
		{
			if (!command || !*command)
			{
				return;
			}

			char buffer[1024]{};

			if (command[0] == '+')
			{
				if (down)
				{
					sprintf_s(buffer, "%s %i %u\n", command, key, time);
				}
				else
				{
					sprintf_s(buffer, "-%s %i %u\n", &command[1], key, time);
				}

				game::Cbuf_AddText(0, buffer);
				return;
			}

			if (down)
			{
				sprintf_s(buffer, "%s\n", command);
				game::Cbuf_AddText(0, buffer);
			}
		}

		void handle_gameplay_button_event(const int key, const bool is_down, const DWORD time)
		{
			if (!key)
			{
				return;
			}

			auto& player_key_state = game::playerKeys[0];
			auto& key_state = player_key_state.keys[key];

			if (is_down)
			{
				key_state.down = 1;
				if (++key_state.repeats == 1)
				{
					player_key_state.anyKeyDown++;
				}
			}
			else
			{
				key_state.down = 0;
				if (key_state.repeats > 0)
				{
					key_state.repeats = 0;
					player_key_state.anyKeyDown = std::max(0, player_key_state.anyKeyDown - 1);
				}
			}

			const auto* command = get_fallback_gameplay_command(key);
			if (!command)
			{
				return;
			}

			// Match IW3SP's behavior closely enough for gameplay buttons:
			// send the bound +command on press and its matching -command on release.
			if (is_down && key_state.repeats > 1)
			{
				return;
			}

			queue_command(key, command, is_down, time);
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
			pad.menu_hold_start_time.fill(0);
		}

		void update_activity(const DWORD time)
		{
			pad.last_activity_time = time;
		}

		void set_cursor_visible(const bool visible)
		{
			if (!game::main_window)
			{
				return;
			}

			const auto hwnd = *game::main_window;
			if (!hwnd || !IsWindow(hwnd))
			{
				return;
			}

			if (visible)
			{
				while (ShowCursor(TRUE) < 0)
				{
				}

				cursor_hidden_for_gamepad = false;
				return;
			}

			while (ShowCursor(FALSE) >= 0)
			{
			}

			SetCursor(nullptr);
			cursor_hidden_for_gamepad = true;
		}

		void update_cursor_visibility(const DWORD time)
		{
			const auto hwnd = game::main_window ? *game::main_window : nullptr;
			if (!hwnd || !IsWindow(hwnd))
			{
				if (cursor_hidden_for_gamepad)
				{
					set_cursor_visible(true);
				}

				return;
			}

			if (GetForegroundWindow() != hwnd)
			{
				if (cursor_hidden_for_gamepad)
				{
					set_cursor_visible(true);
				}

				return;
			}

			const auto controller_active = is_gamepad_enabled()
				&& pad.connected
				&& ((time - pad.last_activity_time) <= 2000u);
			const auto recent_mouse_activity = (time - last_mouse_activity_time.load()) <= 16u;

			if (controller_active && !recent_mouse_activity)
			{
				if (!cursor_hidden_for_gamepad)
				{
					set_cursor_visible(false);
				}
			}
			else if (cursor_hidden_for_gamepad)
			{
				set_cursor_visible(true);
			}
		}

		void handle_menu_direction(const direction dir, const int key, const bool active, const DWORD time)
		{
			auto& is_down = pad.menu_direction_down[dir];
			auto& next_repeat = pad.menu_next_repeat_time[dir];
			auto& hold_start = pad.menu_hold_start_time[dir];

			if (active)
			{
				if (!is_down)
				{
					pulse_key_event(key, time);
					is_down = true;
					hold_start = time;
					next_repeat = time + get_first_repeat_delay();
					update_activity(time);
					return;
				}

				if (time >= next_repeat)
				{
					// Credit: accelerated menu repeat tuning adapted from controller work by GitHub user not-czar.
					auto repeat_delay = get_rest_repeat_delay();
					const auto min_delay = get_min_repeat_delay();
					const auto accel_time = get_repeat_accel_time();
					if (accel_time > 0 && repeat_delay > min_delay)
					{
						const auto held_for = time - hold_start;
						const auto accel_progress = std::min(held_for, accel_time);
						repeat_delay -= (repeat_delay - min_delay) * accel_progress / accel_time;
					}

					// Credit: accelerated menu repeat tuning adapted from controller work by GitHub user not-czar.
					pulse_key_event(key, time);
					next_repeat = time + repeat_delay;
					update_activity(time);
				}

				return;
			}

			is_down = false;
			next_repeat = 0;
			hold_start = 0;
		}

		void handle_menu_navigation(const XINPUT_GAMEPAD& state, const DWORD time)
		{
			const auto threshold = std::max(0.05f, get_stick_deadzone_min() + 0.05f);
			const auto up = (state.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0 || pad.left_stick_y >= threshold;
			const auto down = (state.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 || pad.left_stick_y <= -threshold;
			const auto left = (state.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 || pad.left_stick_x <= -threshold;
			const auto right = (state.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 || pad.left_stick_x >= threshold;

			handle_menu_direction(dir_up, game::K_UPARROW, up, time);
			handle_menu_direction(dir_down, game::K_DOWNARROW, down, time);
			handle_menu_direction(dir_left, game::K_LEFTARROW, left, time);
			handle_menu_direction(dir_right, game::K_RIGHTARROW, right, time);
		}

		void update_analog_state(const XINPUT_GAMEPAD& state)
		{
			float left_stick_x = 0.0f;
			float left_stick_y = 0.0f;
			float right_stick_x = 0.0f;
			float right_stick_y = 0.0f;

			normalize_stick_pair(state.sThumbLX, state.sThumbLY, left_stick_x, left_stick_y);
			normalize_stick_pair(state.sThumbRX, state.sThumbRY, right_stick_x, right_stick_y);

			pad.left_stick_x = left_stick_x;
			pad.left_stick_y = left_stick_y;
			pad.right_stick_x = right_stick_x;
			pad.right_stick_y = right_stick_y;
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
					if (is_dpad_mapping(mapping.xinput_mask))
					{
						continue;
					}

					fire_key_event(mapping.menu_key, is_down, time);
				}
				else
				{
					switch (mapping.gameplay_key)
					{
					case game::K_BUTTON_START:
						fire_key_event(game::K_ESCAPE, is_down, time);
						break;
					case game::K_BUTTON_BACK:
						fire_key_event(game::K_TAB, is_down, time);
						break;
					default:
						handle_gameplay_button_event(mapping.gameplay_key, is_down, time);
						break;
					}
				}

				if (is_down)
				{
					update_activity(time);
				}
			}
		}

		void handle_trigger_button(const float current_value, const float previous_value, const int gameplay_key, const int menu_key, const DWORD time)
		{
			const auto was_down = previous_value > 0.0f;
			const auto is_down = current_value > 0.0f;
			if (was_down == is_down)
			{
				return;
			}

			if (pad.menu_mode)
			{
				fire_key_event(menu_key, is_down, time);
			}
			else
			{
				handle_gameplay_button_event(gameplay_key, is_down, time);
			}

			if (is_down)
			{
				update_activity(time);
			}
		}

		enum gamepad_physical_axis
		{
			GPAD_PHYSAXIS_NONE = -1,
			GPAD_PHYSAXIS_RSTICK_X = 0,
			GPAD_PHYSAXIS_RSTICK_Y,
			GPAD_PHYSAXIS_LSTICK_X,
			GPAD_PHYSAXIS_LSTICK_Y,
			GPAD_PHYSAXIS_RTRIGGER,
			GPAD_PHYSAXIS_LTRIGGER,
			GPAD_PHYSAXIS_COUNT
		};

		enum gamepad_virtual_axis
		{
			GPAD_VIRTAXIS_NONE = -1,
			GPAD_VIRTAXIS_SIDE = 0,
			GPAD_VIRTAXIS_FORWARD,
			GPAD_VIRTAXIS_UP,
			GPAD_VIRTAXIS_YAW,
			GPAD_VIRTAXIS_PITCH,
			GPAD_VIRTAXIS_ATTACK,
			GPAD_VIRTAXIS_COUNT
		};

		enum gamepad_mapping
		{
			GPAD_MAP_NONE = 0,
			GPAD_MAP_LINEAR,
			GPAD_MAP_SQUARED
		};

		struct gamepad_axis_binding
		{
			gamepad_physical_axis physical_axis;
			gamepad_mapping map_type;
		};

		constexpr gamepad_physical_axis axis_same_stick[GPAD_PHYSAXIS_COUNT]
		{
			GPAD_PHYSAXIS_RSTICK_Y,
			GPAD_PHYSAXIS_RSTICK_X,
			GPAD_PHYSAXIS_LSTICK_Y,
			GPAD_PHYSAXIS_LSTICK_X,
			GPAD_PHYSAXIS_NONE,
			GPAD_PHYSAXIS_NONE
		};

		constexpr gamepad_axis_binding virtual_axis_bindings[GPAD_VIRTAXIS_COUNT]
		{
			{ GPAD_PHYSAXIS_LSTICK_X, GPAD_MAP_SQUARED },
			{ GPAD_PHYSAXIS_LSTICK_Y, GPAD_MAP_SQUARED },
			{ GPAD_PHYSAXIS_NONE, GPAD_MAP_NONE },
			{ GPAD_PHYSAXIS_RSTICK_X, GPAD_MAP_LINEAR },
			{ GPAD_PHYSAXIS_RSTICK_Y, GPAD_MAP_LINEAR },
			{ GPAD_PHYSAXIS_NONE, GPAD_MAP_NONE }
		};

		std::int8_t clamp_cmd_axis(const int value)
		{
			return static_cast<std::int8_t>(std::clamp(value, -127, 127));
		}

		float get_physical_axis_value(const gamepad_physical_axis axis)
		{
			switch (axis)
			{
			case GPAD_PHYSAXIS_RSTICK_X:
				return pad.right_stick_x;
			case GPAD_PHYSAXIS_RSTICK_Y:
				return pad.right_stick_y;
			case GPAD_PHYSAXIS_LSTICK_X:
				return pad.left_stick_x;
			case GPAD_PHYSAXIS_LSTICK_Y:
				return pad.left_stick_y;
			case GPAD_PHYSAXIS_RTRIGGER:
				return pad.right_trigger;
			case GPAD_PHYSAXIS_LTRIGGER:
				return pad.left_trigger;
			default:
				return 0.0f;
			}
		}

		float CL_GamepadAxisValue(const gamepad_virtual_axis virtual_axis)
		{
			assert(virtual_axis > GPAD_VIRTAXIS_NONE && virtual_axis < GPAD_VIRTAXIS_COUNT);

			const auto& [physical_axis, map_type] = virtual_axis_bindings[virtual_axis];

			if (physical_axis <= GPAD_PHYSAXIS_NONE || physical_axis >= GPAD_PHYSAXIS_COUNT)
			{
				return 0.0f;
			}

			auto axis_deflection = get_physical_axis_value(physical_axis);

			if (map_type == GPAD_MAP_SQUARED)
			{
				const auto other_axis_same_stick = axis_same_stick[physical_axis];

				float other_axis_deflection;
				if (other_axis_same_stick <= GPAD_PHYSAXIS_NONE || other_axis_same_stick >= GPAD_PHYSAXIS_COUNT)
				{
					other_axis_deflection = 0.0f;
				}
				else
				{
					other_axis_deflection = get_physical_axis_value(other_axis_same_stick);
				}

				axis_deflection = std::sqrt(axis_deflection * axis_deflection + other_axis_deflection * other_axis_deflection) * axis_deflection;
			}

			return axis_deflection;
		}

		void CL_GamepadMove(game::usercmd_t* cmd)
		{
			if (!cmd || !should_drive_native_cmd())
			{
				return;
			}

			const auto forward = CL_GamepadAxisValue(GPAD_VIRTAXIS_FORWARD);
			const auto side = CL_GamepadAxisValue(GPAD_VIRTAXIS_SIDE);
			constexpr auto move_scale = 127.0f;

			// Preserve true analog walking by mapping normalized stick deflection
			// directly into the signed usercmd movement range.
			cmd->rightmove = clamp_cmd_axis(static_cast<int>(std::lround(side * move_scale)));
			cmd->forwardmove = clamp_cmd_axis(static_cast<int>(std::lround(forward * move_scale)));

		}

		float get_view_sensitivity()
		{
			const auto* const dvar = game::Dvar_FindVar("input_viewSensitivity");
			return dvar ? std::clamp(dvar->current.value, 0.01f, 30.0f) : 1.0f;
		}

		float get_turn_rate(const char* normal_name, const char* ads_name, const float normal_default, const float ads_default, const bool ads_active)
		{
			const auto* const dvar = game::Dvar_FindVar(ads_active ? ads_name : normal_name);
			if (!dvar)
			{
				return ads_active ? ads_default : normal_default;
			}

			return std::clamp(dvar->current.value, 1.0f, 2000.0f);
		}

		float* get_native_pitch_offset()
		{
			return reinterpret_cast<float*>(game::game_offset(0x11A9FEC0));
		}

		float* get_native_yaw_offset()
		{
			return reinterpret_cast<float*>(game::game_offset(0x11A9FEC4));
		}

		bool should_hide_ui_cursor()
		{
			if (!should_hide_cursor_now())
			{
				return false;
			}

			return !game_console::is_active();
		}

		bool __cdecl draw_crosshair_body()
		{
			const auto func = reinterpret_cast<bool(__cdecl*)()>(game::game_offset(0x102DA010));
			const auto drew_crosshair = func();

			if (!drew_crosshair && should_hide_ui_cursor())
			{
				return true;
			}

			return drew_crosshair;
		}

		void apply_native_view_input()
		{
			if (!should_drive_native_cmd())
			{
				return;
			}

			const auto yaw_axis = -pad.right_stick_x;
			auto pitch_axis = pad.right_stick_y;

			if (!dvars::input_invertPitch || !dvars::input_invertPitch->current.enabled)
			{
				pitch_axis *= -1.0f;
			}

			if (std::fabs(yaw_axis) <= 0.0f && std::fabs(pitch_axis) <= 0.0f)
			{
				return;
			}

			const auto ads_active = pad.left_trigger > 0.0f;
			const auto sensitivity = get_view_sensitivity();
			const auto yaw_rate = get_turn_rate("cl_yawspeed", "cl_yawspeed_ads", 140.0f, 90.0f, ads_active) * sensitivity;
			const auto pitch_rate = get_turn_rate("cl_pitchspeed", "cl_pitchspeed_ads", 140.0f, 90.0f, ads_active) * sensitivity;

			auto* const pitch = get_native_pitch_offset();
			auto* const yaw = get_native_yaw_offset();
			if (!pitch || !yaw)
			{
				return;
			}

			*pitch = std::clamp(*pitch + (pitch_axis * pitch_rate * analog_frame_seconds), -85.0f, 85.0f);
			*yaw += yaw_axis * yaw_rate * analog_frame_seconds;
		}

		void apply_native_gamepad_to_cmd(game::usercmd_t* cmd)
		{
			if (!cmd || !should_drive_native_cmd())
			{
				return;
			}

			CL_GamepadMove(cmd);
		}

		double get_move_axis_value(const gamepad_virtual_axis axis, const bool positive)
		{
			if (!should_drive_native_cmd())
			{
				return 0.0;
			}

			// When native controller movement is active, we write analog movement
			// straight into usercmd_t and suppress the older split-axis path.
			(void)axis;
			(void)positive;
			return 0.0;
		}

		double move_right_positive_body()
		{
			return get_move_axis_value(GPAD_VIRTAXIS_SIDE, true);
		}

		double move_right_negative_body()
		{
			return get_move_axis_value(GPAD_VIRTAXIS_SIDE, false);
		}

		double move_forward_positive_body()
		{
			return get_move_axis_value(GPAD_VIRTAXIS_FORWARD, true);
		}

		double move_forward_negative_body()
		{
			return get_move_axis_value(GPAD_VIRTAXIS_FORWARD, false);
		}

		game::usercmd_t* __cdecl build_cmd_body(game::usercmd_t* cmd, const int local_client_num)
		{
			// This is the stock QoS usercmd builder reached from 0x102FFCD4.
			// Let the game build its normal command first.
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
			patches::enforce_ads_sprint_interrupt(result);

			return result;
		}

		void __cdecl native_look_body(game::usercmd_t* cmd, const int local_client_num)
		{
			const auto func_loc = static_cast<int>(game::game_offset(0x102FC4D0));

			if (should_drive_native_cmd())
			{
				apply_native_view_input();
				return;
			}

			__asm
			{
				push cmd
				mov eax, local_client_num
				call func_loc
				add esp, 4
			}
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

		__declspec(naked) void native_look_stub()
		{
			__asm
			{
				mov edx, [esp + 4]
				push eax
				push edx
				call native_look_body
				add esp, 8
				ret
			}
		}

		__declspec(naked) void pack_current_move_stub()
		{
			__asm
			{
				mov al, byte ptr [ebp + 1Ch]
				mov ah, byte ptr [ebp + 1Eh]
				movzx ecx, ax
				mov dword ptr [esp + 0Ch], ecx
				mov eax, pack_current_move_rejoin
				jmp eax
			}
		}

		__declspec(naked) void pack_previous_move_stub()
		{
			__asm
			{
				mov al, byte ptr [edi + 1Ch]
				mov ah, byte ptr [edi + 1Eh]
				movzx ecx, ax
				mov dword ptr [esp + 1Ch], ecx
				mov eax, pack_previous_move_rejoin
				jmp eax
			}
		}

		__declspec(naked) void unpack_base_move_a_stub()
		{
			__asm
			{
				mov al, byte ptr [ebx + 1Ch]
				mov ah, byte ptr [ebx + 1Eh]
				movzx ecx, ax
				mov dword ptr [esp + 10h], ecx
				mov eax, unpack_base_move_a_rejoin
				jmp eax
			}
		}

		__declspec(naked) void unpack_base_move_b_stub()
		{
			__asm
			{
				mov al, byte ptr [ebx + 1Ch]
				mov ah, byte ptr [ebx + 1Eh]
				movzx ecx, ax
				mov dword ptr [esp + 10h], ecx
				mov eax, unpack_base_move_b_rejoin
				jmp eax
			}
		}

		__declspec(naked) void unpack_exact_move_a_stub()
		{
			__asm
			{
				mov byte ptr [edi + 1Ch], al
				mov byte ptr [edi + 1Eh], ah
				mov eax, unpack_exact_move_a_rejoin
				jmp eax
			}
		}

		__declspec(naked) void unpack_exact_move_b_stub()
		{
			__asm
			{
				mov byte ptr [edi + 1Ch], al
				mov byte ptr [edi + 1Eh], ah
				mov eax, unpack_exact_move_b_rejoin
				jmp eax
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

		void install_native_look_hook()
		{
			if (native_look_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_native_look_call.size(); ++i)
			{
				original_native_look_call[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFBFE + i));
			}

			utils::hook::call(game::game_offset(0x102FFBFE), native_look_stub);
			native_look_callsite_patched = true;
		}

		void install_draw_crosshair_hook()
		{
			if (draw_crosshair_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_draw_crosshair_call.size(); ++i)
			{
				original_draw_crosshair_call[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102DFBAB + i));
			}

			utils::hook::call(game::game_offset(0x102DFBAB), draw_crosshair_body);
			draw_crosshair_callsite_patched = true;
		}

		void install_move_axis_hooks()
		{
			if (move_axis_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_move_axis_call_1.size(); ++i)
			{
				original_move_axis_call_1[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFA54 + static_cast<int>(i)));
				original_move_axis_call_2[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFA69 + static_cast<int>(i)));
				original_move_axis_call_3[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFA7E + static_cast<int>(i)));
				original_move_axis_call_4[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFA95 + static_cast<int>(i)));
			}

			utils::hook::call(game::game_offset(0x102FFA54), move_right_positive_body);
			utils::hook::call(game::game_offset(0x102FFA69), move_right_negative_body);
			utils::hook::call(game::game_offset(0x102FFA7E), move_forward_positive_body);
			utils::hook::call(game::game_offset(0x102FFA95), move_forward_negative_body);
			move_axis_callsite_patched = true;
		}

		void install_usercmd_movement_patch()
		{
			if (usercmd_movement_patched)
			{
				return;
			}

			pack_current_move_rejoin = game::game_offset(0x103EF8A7);
			pack_previous_move_rejoin = game::game_offset(0x103EF8E9);
			unpack_base_move_a_rejoin = game::game_offset(0x103F02BD);
			unpack_base_move_b_rejoin = game::game_offset(0x103F03EB);
			unpack_exact_move_a_rejoin = game::game_offset(0x103F0310);
			unpack_exact_move_b_rejoin = game::game_offset(0x103F043E);

			for (auto i = 0u; i < 5; ++i)
			{
				original_pack_current_move_jump[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x103EF865 + static_cast<int>(i)));
				original_pack_previous_move_jump[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x103EF8A7 + static_cast<int>(i)));
				original_unpack_base_move_a_jump[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x103F027B + static_cast<int>(i)));
				original_unpack_base_move_b_jump[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x103F03A9 + static_cast<int>(i)));
				original_unpack_exact_move_a_jump[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x103F02E2 + static_cast<int>(i)));
				original_unpack_exact_move_b_jump[i] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x103F0410 + static_cast<int>(i)));
			}

			original_write_move_bits_a = utils::hook::get<std::uint16_t>(game::game_offset(0x103EF9CE));
			original_write_move_bits_b = utils::hook::get<std::uint16_t>(game::game_offset(0x103EFA44));
			original_read_move_bits_a = utils::hook::get<std::uint16_t>(game::game_offset(0x103F02C8));
			original_read_move_bits_b = utils::hook::get<std::uint16_t>(game::game_offset(0x103F03F6));

			utils::hook::jump(game::game_offset(0x103EF865), pack_current_move_stub);
			utils::hook::jump(game::game_offset(0x103EF8A7), pack_previous_move_stub);
			utils::hook::jump(game::game_offset(0x103F027B), unpack_base_move_a_stub);
			utils::hook::jump(game::game_offset(0x103F03A9), unpack_base_move_b_stub);
			utils::hook::jump(game::game_offset(0x103F02E2), unpack_exact_move_a_stub);
			utils::hook::jump(game::game_offset(0x103F0410), unpack_exact_move_b_stub);

			utils::hook::set<std::uint16_t>(game::game_offset(0x103EF9CE), 0x106A);
			utils::hook::set<std::uint16_t>(game::game_offset(0x103EFA44), 0x106A);
			utils::hook::set<std::uint16_t>(game::game_offset(0x103F02C8), 0x106A);
			utils::hook::set<std::uint16_t>(game::game_offset(0x103F03F6), 0x106A);

			usercmd_movement_patched = true;
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

		void restore_native_look_hook()
		{
			if (!native_look_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_native_look_call.size(); ++i)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFBFE + static_cast<int>(i)), original_native_look_call[i]);
			}

			native_look_callsite_patched = false;
		}

		void restore_draw_crosshair_hook()
		{
			if (!draw_crosshair_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_draw_crosshair_call.size(); ++i)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x102DFBAB + static_cast<int>(i)), original_draw_crosshair_call[i]);
			}

			draw_crosshair_callsite_patched = false;
		}

		void restore_move_axis_hooks()
		{
			if (!move_axis_callsite_patched)
			{
				return;
			}

			for (auto i = 0u; i < original_move_axis_call_1.size(); ++i)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFA54 + static_cast<int>(i)), original_move_axis_call_1[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFA69 + static_cast<int>(i)), original_move_axis_call_2[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFA7E + static_cast<int>(i)), original_move_axis_call_3[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFA95 + static_cast<int>(i)), original_move_axis_call_4[i]);
			}

			move_axis_callsite_patched = false;
		}

		void restore_usercmd_movement_patch()
		{
			if (!usercmd_movement_patched)
			{
				return;
			}

			for (auto i = 0u; i < 5; ++i)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x103EF865 + static_cast<int>(i)), original_pack_current_move_jump[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103EF8A7 + static_cast<int>(i)), original_pack_previous_move_jump[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F027B + static_cast<int>(i)), original_unpack_base_move_a_jump[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F03A9 + static_cast<int>(i)), original_unpack_base_move_b_jump[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F02E2 + static_cast<int>(i)), original_unpack_exact_move_a_jump[i]);
				utils::hook::set<std::uint8_t>(game::game_offset(0x103F0410 + static_cast<int>(i)), original_unpack_exact_move_b_jump[i]);
			}

			utils::hook::set<std::uint16_t>(game::game_offset(0x103EF9CE), original_write_move_bits_a);
			utils::hook::set<std::uint16_t>(game::game_offset(0x103EFA44), original_write_move_bits_b);
			utils::hook::set<std::uint16_t>(game::game_offset(0x103F02C8), original_read_move_bits_a);
			utils::hook::set<std::uint16_t>(game::game_offset(0x103F03F6), original_read_move_bits_b);

			usercmd_movement_patched = false;
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
				last_analog_update_time = 0;
				analog_frame_seconds = 1.0f / 60.0f;
				set_bool_dvar(dvars::gpad_in_use, false);
				return;
			}

			if (last_analog_update_time != 0)
			{
				const auto delta_ms = time - last_analog_update_time;
				analog_frame_seconds = std::clamp(static_cast<float>(delta_ms) / 1000.0f, 1.0f / 250.0f, 1.0f / 20.0f);
			}
			else
			{
				analog_frame_seconds = 1.0f / 60.0f;
			}

			last_analog_update_time = time;

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
			handle_trigger_button(
				pad.left_trigger,
				normalize_trigger(pad.previous_state.Gamepad.bLeftTrigger),
				game::K_BUTTON_LTRIG,
				0,
				time
			);
			handle_trigger_button(
				pad.right_trigger,
				normalize_trigger(pad.previous_state.Gamepad.bRightTrigger),
				game::K_BUTTON_RTRIG,
				0,
				time
			);

			if (pad.menu_mode)
			{
				handle_menu_navigation(pad.state.Gamepad, time);
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
			update_cursor_visibility(time);

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

	void record_mouse_activity()
	{
		last_mouse_activity_time = GetTickCount();
		if (cursor_hidden_for_gamepad)
		{
			set_cursor_visible(true);
		}
	}

	bool should_hide_cursor_now()
	{
		return cursor_hidden_for_gamepad;
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			install_native_cmd_hook();
			install_native_look_hook();
			install_draw_crosshair_hook();
			install_move_axis_hooks();

			scheduler::loop([]()
			{
				poll_controller();
			}, scheduler::main, 16ms);

			scheduler::on_shutdown([]()
			{
				shutdown_requested = true;
				release_all_inputs(GetTickCount());
				if (cursor_hidden_for_gamepad)
				{
					set_cursor_visible(true);
				}
				restore_native_cmd_hook();
				restore_native_look_hook();
				restore_draw_crosshair_hook();
				restore_move_axis_hooks();
				set_bool_dvar(dvars::gpad_present, false);
				set_bool_dvar(dvars::gpad_in_use, false);
			});
		}

		void pre_destroy() override
		{
			shutdown_requested = true;
			restore_native_cmd_hook();
			restore_native_look_hook();
			restore_draw_crosshair_hook();
			restore_move_axis_hooks();
		}
	};
}

namespace gamepad
{
	bool is_controller_active()
	{
		if (!dvars::gpad_in_use)
		{
			return false;
		}

		return dvars::gpad_in_use->current.enabled;
	}

	bool should_hide_cursor()
	{
		return xinput::should_hide_cursor_now();
	}

	void note_mouse_activity()
	{
		xinput::record_mouse_activity();
	}
}

REGISTER_COMPONENT(xinput::component)
