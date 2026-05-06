#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/utils/scheduler.hpp"
#include "gamepad.hpp"
#include "component/engine/console/game_console.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <intrin.h>
#include <Xinput.h>

#include <utils/hook.hpp>

#pragma comment(lib, "xinput9_1_0.lib")

namespace iw4xinput
{
	namespace
	{
		struct controller_state
		{
			bool connected = false;
			bool in_use = false;
			bool menu_mode = false;
			XINPUT_STATE state{};
			XINPUT_STATE previous_state{};
			std::array<bool, 4> menu_direction_down{};
			std::array<DWORD, 4> menu_next_repeat_time{};
			std::array<DWORD, 4> menu_hold_start_time{};
			DWORD last_activity_time = 0;
			float sticks[4]{};
			float last_sticks[4]{};
			float digitals = 0.0f;
			float left_trigger = 0.0f;
			float right_trigger = 0.0f;
		};

		controller_state pad{};
		bool shutdown_requested = false;
		bool key_move_callsite_patched = false;
		bool mouse_move_callsite_patched = false;
		std::array<std::uint8_t, 5> original_key_move_call_1{};
		std::array<std::uint8_t, 5> original_key_move_call_2{};
		std::array<std::uint8_t, 5> original_key_move_call_3{};
		std::array<std::uint8_t, 5> original_key_move_call_4{};
		std::array<std::uint8_t, 5> original_key_move_call_5{};
		std::array<std::uint8_t, 5> original_key_move_call_6{};
		std::array<std::uint8_t, 5> original_mouse_move_call{};
		std::atomic<DWORD> last_mouse_activity_time{ 0 };
		bool cursor_hidden_for_gamepad = false;
		DWORD last_update_time = 0;
		float frame_seconds = 1.0f / 60.0f;
		std::uint32_t mouse_move_rejoin = 0;

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

		bool is_gamepad_enabled()
		{
			return dvars::gpad_enabled && dvars::gpad_enabled->current.enabled;
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
				&& !shutdown_requested
				&& !is_menu_mode();
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

		void set_in_use(const bool value)
		{
			pad.in_use = value;
			set_bool_dvar(dvars::gpad_in_use, value);
		}

		void release_all_inputs(const DWORD time)
		{
			(void)time;
			pad.menu_direction_down.fill(false);
			pad.menu_next_repeat_time.fill(0);
			pad.menu_hold_start_time.fill(0);
		}

		void update_activity(const DWORD time)
		{
			pad.last_activity_time = time;
			set_in_use(true);
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
			if (!hwnd || !IsWindow(hwnd) || GetForegroundWindow() != hwnd)
			{
				if (cursor_hidden_for_gamepad)
				{
					set_cursor_visible(true);
				}
				return;
			}

			const auto controller_active = is_gamepad_enabled() && pad.connected && pad.in_use;
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

		void fire_key_event(const int key, const bool down, const DWORD time)
		{
			if (!key)
			{
				return;
			}

			game::CL_KeyEvent(0, key, down ? 1 : 0, time);
		}

		void handle_button(const WORD mask, const int gameplay_key, const int menu_key, const bool is_down, const DWORD time)
		{
			(void)mask;
			if (is_down)
			{
				update_activity(time);
			}

			if (pad.menu_mode)
			{
				fire_key_event(menu_key, is_down, time);
				return;
			}

			switch (gameplay_key)
			{
			case game::K_BUTTON_START:
				fire_key_event(game::K_ESCAPE, is_down, time);
				break;
			case game::K_BUTTON_BACK:
				fire_key_event(game::K_TAB, is_down, time);
				break;
			default:
				fire_key_event(gameplay_key, is_down, time);
				break;
			}
		}

		float normalize_axis_component(const SHORT value)
		{
			return static_cast<float>(value) / static_cast<float>(std::numeric_limits<SHORT>::max());
		}

		void GPad_ConvertStickToFloat(const short x, const short y, float& out_x, float& out_y)
		{
			if (x == 0 && y == 0)
			{
				out_x = 0.0f;
				out_y = 0.0f;
				return;
			}

			float stick_vec[2];
			stick_vec[0] = normalize_axis_component(x);
			stick_vec[1] = normalize_axis_component(y);

			const auto deadzone_min = dvars::gpad_stick_deadzone_min ? dvars::gpad_stick_deadzone_min->current.value : 0.0f;
			const auto deadzone_max = dvars::gpad_stick_deadzone_max ? dvars::gpad_stick_deadzone_max->current.value : 0.0f;
			const auto dead_zone_total = deadzone_min + deadzone_max;
			const auto len = std::sqrt((stick_vec[0] * stick_vec[0]) + (stick_vec[1] * stick_vec[1]));
			auto scaled_len = 0.0f;

			if (deadzone_min <= len)
			{
				if (1.0f - deadzone_max >= len)
				{
					scaled_len = (len - deadzone_min) / (1.0f - dead_zone_total);
				}
				else
				{
					scaled_len = 1.0f;
				}
			}

			const auto inv_len = 1.0f / std::max(len, 0.0001f);
			out_x = stick_vec[0] * inv_len * scaled_len;
			out_y = stick_vec[1] * inv_len * scaled_len;
		}

		void GPad_UpdateSticks(const XINPUT_GAMEPAD& state)
		{
			float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
			GPad_ConvertStickToFloat(state.sThumbLX, state.sThumbLY, lx, ly);
			GPad_ConvertStickToFloat(state.sThumbRX, state.sThumbRY, rx, ry);

			pad.last_sticks[0] = pad.sticks[0];
			pad.sticks[0] = lx;
			pad.last_sticks[1] = pad.sticks[1];
			pad.sticks[1] = ly;
			pad.last_sticks[2] = pad.sticks[2];
			pad.sticks[2] = rx;
			pad.last_sticks[3] = pad.sticks[3];
			pad.sticks[3] = ry;
		}

		float CL_GamepadAxisValue(const gamepad_virtual_axis virtual_axis)
		{
			const auto& binding = virtual_axis_bindings[virtual_axis];
			if (binding.physical_axis <= GPAD_PHYSAXIS_NONE || binding.physical_axis >= GPAD_PHYSAXIS_COUNT)
			{
				return 0.0f;
			}

			auto axis_deflection = pad.sticks[binding.physical_axis];
			if (binding.map_type == GPAD_MAP_SQUARED)
			{
				const auto other_axis_same_stick = axis_same_stick[binding.physical_axis];
				const auto other = (other_axis_same_stick > GPAD_PHYSAXIS_NONE && other_axis_same_stick < GPAD_PHYSAXIS_COUNT)
					? pad.sticks[other_axis_same_stick]
					: 0.0f;
				axis_deflection = std::sqrt(axis_deflection * axis_deflection + other * other) * axis_deflection;
			}

			return axis_deflection;
		}

		char clamp_char(const int value)
		{
			return static_cast<char>(std::clamp<int>(value,
				static_cast<int>(std::numeric_limits<char>::min()),
				static_cast<int>(std::numeric_limits<char>::max())));
		}

		enum class keymove_axis_slot
		{
			none,
			right_positive,
			right_negative,
			forward_positive,
			forward_negative
		};

		keymove_axis_slot get_keymove_axis_slot(const std::uintptr_t return_address)
		{
			switch (return_address)
			{
			case 0x102FFA59:
			case 0x102FFB15:
				return keymove_axis_slot::right_positive;
			case 0x102FFA6E:
			case 0x102FFB29:
				return keymove_axis_slot::right_negative;
			case 0x102FFA83:
				return keymove_axis_slot::forward_positive;
			case 0x102FFA9A:
				return keymove_axis_slot::forward_negative;
			default:
				return keymove_axis_slot::none;
			}
		}

		float get_controller_keymove_value(const keymove_axis_slot slot)
		{
			switch (slot)
			{
			case keymove_axis_slot::none:
				return 0.0f;
			case keymove_axis_slot::right_positive:
				return std::max(0.0f, pad.sticks[0]);
			case keymove_axis_slot::right_negative:
				return std::max(0.0f, -pad.sticks[0]);
			case keymove_axis_slot::forward_positive:
				return std::max(0.0f, pad.sticks[1]);
			case keymove_axis_slot::forward_negative:
				return std::max(0.0f, -pad.sticks[1]);
			default:
				return 0.0f;
			}
		}

		float __cdecl cl_keymove_axis_body_impl(const int key_state_ptr, const std::uintptr_t return_address)
		{
			double base_value = 0.0;
			const auto original = game::game_offset(0x102FB1F0);
			__asm
			{
				mov eax, key_state_ptr
				call original
				fstp qword ptr [base_value]
			}

			if (!is_gamepad_enabled() || !pad.connected)
			{
				return static_cast<float>(base_value);
			}

			const auto controller_value = get_controller_keymove_value(get_keymove_axis_slot(return_address));
			return controller_value > 0.0f ? controller_value : static_cast<float>(base_value);
		}

		__declspec(naked) float cl_keymove_axis_body()
		{
			__asm
			{
				mov edx, [esp]
				push edx
				push eax
				call cl_keymove_axis_body_impl
				add esp, 8
				retn
			}
		}

		void CL_GamepadLook(game::usercmd_t* cmd)
		{
			if (!cmd || !is_gamepad_enabled() || !pad.connected)
			{
				return;
			}

			auto pitch = CL_GamepadAxisValue(GPAD_VIRTAXIS_PITCH);
			if (!dvars::input_invertPitch || !dvars::input_invertPitch->current.enabled)
			{
				pitch *= -1.0f;
			}

			auto yaw = -CL_GamepadAxisValue(GPAD_VIRTAXIS_YAW);
			const auto yaw_rate = 140.0f;
			const auto pitch_rate = 140.0f;

			cmd->angles[0] += static_cast<int>(std::floor(pitch * pitch_rate * frame_seconds));
			cmd->angles[1] += static_cast<int>(std::floor(yaw * yaw_rate * frame_seconds));
		}

		void CL_MouseMove(game::usercmd_t* cmd, const int local_client_num)
		{
			const auto original = reinterpret_cast<void(__cdecl*)(game::usercmd_t*)>(game::game_offset(0x102FC4D0));
			if (should_drive_native_cmd())
			{
				CL_GamepadLook(cmd);
				return;
			}

			__asm
			{
				push cmd
				mov eax, local_client_num
				call original
				add esp, 4
			}
		}

		void install_mouse_move_hook()
		{
			if (mouse_move_callsite_patched)
			{
				return;
			}

			for (auto j = 0u; j < original_mouse_move_call.size(); ++j)
			{
				original_mouse_move_call[j] = *reinterpret_cast<std::uint8_t*>(game::game_offset(0x102FFBFE + j));
			}

			utils::hook::call(game::game_offset(0x102FFBFE), CL_MouseMove);
			mouse_move_callsite_patched = true;
		}

		void restore_mouse_move_hook()
		{
			if (!mouse_move_callsite_patched)
			{
				return;
			}

			for (auto j = 0u; j < original_mouse_move_call.size(); ++j)
			{
				utils::hook::set<std::uint8_t>(game::game_offset(0x102FFBFE + j), original_mouse_move_call[j]);
			}

			mouse_move_callsite_patched = false;
		}

		void install_key_move_hook()
		{
			if (key_move_callsite_patched)
			{
				return;
			}

			constexpr std::array<std::uint32_t, 6> callsites
			{
				0x102FFA54,
				0x102FFA69,
				0x102FFA7E,
				0x102FFA95,
				0x102FFB10,
				0x102FFB24
			};
			std::array<std::uint8_t, 5>* originals[]
			{
				&original_key_move_call_1,
				&original_key_move_call_2,
				&original_key_move_call_3,
				&original_key_move_call_4,
				&original_key_move_call_5,
				&original_key_move_call_6
			};

			for (size_t i = 0; i < callsites.size(); ++i)
			{
				for (auto j = 0u; j < originals[i]->size(); ++j)
				{
					(*originals[i])[j] = *reinterpret_cast<std::uint8_t*>(game::game_offset(callsites[i] + j));
				}

				utils::hook::call(game::game_offset(callsites[i]), cl_keymove_axis_body);
			}

			key_move_callsite_patched = true;
		}

		void restore_key_move_hook()
		{
			if (!key_move_callsite_patched)
			{
				return;
			}

			constexpr std::array<std::uint32_t, 6> callsites
			{
				0x102FFA54,
				0x102FFA69,
				0x102FFA7E,
				0x102FFA95,
				0x102FFB10,
				0x102FFB24
			};
			const std::array<std::uint8_t, 5>* originals[]
			{
				&original_key_move_call_1,
				&original_key_move_call_2,
				&original_key_move_call_3,
				&original_key_move_call_4,
				&original_key_move_call_5,
				&original_key_move_call_6
			};

			for (size_t i = 0; i < callsites.size(); ++i)
			{
				for (auto j = 0u; j < originals[i]->size(); ++j)
				{
					utils::hook::set<std::uint8_t>(game::game_offset(callsites[i] + j), (*originals[i])[j]);
				}
			}

			key_move_callsite_patched = false;
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
				pad = {};
				set_in_use(false);
				last_update_time = 0;
				frame_seconds = 1.0f / 60.0f;
				return;
			}

			if (last_update_time != 0)
			{
				frame_seconds = std::clamp(static_cast<float>(time - last_update_time) / 1000.0f, 1.0f / 250.0f, 1.0f / 20.0f);
			}

			last_update_time = time;
			pad.connected = true;
			pad.previous_state = pad.state;
			pad.state = state;
			pad.menu_mode = is_menu_mode();

			GPad_UpdateSticks(pad.state.Gamepad);

			const auto moved = std::fabs(pad.sticks[0]) > 0.0f || std::fabs(pad.sticks[1]) > 0.0f
				|| std::fabs(pad.sticks[2]) > 0.0f || std::fabs(pad.sticks[3]) > 0.0f
				|| pad.state.Gamepad.wButtons != 0
				|| pad.state.Gamepad.bLeftTrigger > 0
				|| pad.state.Gamepad.bRightTrigger > 0;

			if (moved)
			{
				update_activity(time);
			}
			else
			{
				set_in_use(false);
			}

			handle_button(XINPUT_GAMEPAD_A, game::K_BUTTON_A, game::K_ENTER, (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0, time);
			handle_button(XINPUT_GAMEPAD_B, game::K_BUTTON_B, game::K_ESCAPE, (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0, time);
			handle_button(XINPUT_GAMEPAD_START, game::K_BUTTON_START, game::K_ENTER, (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0, time);
			handle_button(XINPUT_GAMEPAD_BACK, game::K_BUTTON_BACK, game::K_ESCAPE, (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0, time);

			update_cursor_visibility(time);
			pad.previous_state = pad.state;
		}

		void record_mouse_activity()
		{
			last_mouse_activity_time = GetTickCount();
			set_in_use(false);
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
				install_key_move_hook();
				install_mouse_move_hook();
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
					set_bool_dvar(dvars::gpad_present, false);
					set_bool_dvar(dvars::gpad_in_use, false);
				});
			}

			void pre_destroy() override
			{
				restore_key_move_hook();
				restore_mouse_move_hook();
				shutdown_requested = true;
			}
		};
	}
	bool is_controller_active()
	{
		return pad.in_use;
	}

	bool should_hide_cursor_now()
	{
		return cursor_hidden_for_gamepad;
	}

	void record_mouse_activity()
	{
		last_mouse_activity_time = GetTickCount();
		set_in_use(false);
		if (cursor_hidden_for_gamepad)
		{
			set_cursor_visible(true);
		}
	}
}

namespace gamepad
{
	bool is_controller_active()
	{
		return iw4xinput::is_controller_active();
	}

	bool should_hide_cursor()
	{
		return iw4xinput::should_hide_cursor_now();
	}

	void note_mouse_activity()
	{
		iw4xinput::record_mouse_activity();
	}
}

REGISTER_COMPONENT(iw4xinput::component)
