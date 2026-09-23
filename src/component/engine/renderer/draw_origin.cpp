#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/game_console.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/string.hpp>

namespace draw_origin
{
	namespace
	{
		constexpr auto cg_initialized_address = 0x129FE8E4;
		constexpr auto player_origin_address = 0x12A4CE1C;
		constexpr auto player_velocity_address = 0x12A4CE28;
		constexpr float native_line_spacing = 0.75f;
		constexpr float native_text_y_scale = 1.1f;
		constexpr float right_margin = 4.0f;
		constexpr auto console_update_interval = 500ms;

		float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.75f};
#ifdef DEBUG
		std::atomic_bool internal_console_status_enabled = false;
#endif

		bool read_player_motion(float (&origin)[3], float (&velocity)[3])
		{
			__try
			{
				if (!*reinterpret_cast<std::uintptr_t*>(game::game_offset(cg_initialized_address)))
				{
					return false;
				}

				const auto* const source_origin = reinterpret_cast<const float*>(game::game_offset(player_origin_address));
				const auto* const source_velocity = reinterpret_cast<const float*>(game::game_offset(player_velocity_address));
				for (auto index = 0; index < 3; ++index)
				{
					origin[index] = source_origin[index];
					velocity[index] = source_velocity[index];
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void draw_right_aligned(const char* text, game::Font_s* font, const float right, const float y)
		{
			const auto width = static_cast<float>(game::R_TextWidth(text, 0x7FFFFFFF, font));
			const auto x = right - width - right_margin;
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x + 1.0f, y + 1.0f,
				1.0f, native_text_y_scale, 0.0f, shadow_color, 0);
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x, y,
				1.0f, native_text_y_scale, 0.0f, text_color, 0);
		}

		int fps_line_count()
		{
			const auto* const draw_fps = game::Dvar_FindVar("cg_drawFPS");
			if (!draw_fps || draw_fps->current.integer <= 0)
			{
				return 0;
			}

			return draw_fps->current.integer == 1 ? 1 : 3;
		}

		const char* format_motion(const float (&origin)[3], const float (&velocity)[3])
		{
			return utils::string::va(
				"origin: (%.2f, %.2f, %.2f) velocity: (%.2f, %.2f, %.2f)",
				origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2]);
		}

#ifdef DEBUG
		void update_internal_console_status()
		{
			if (!internal_console_status_enabled.load(std::memory_order_relaxed))
			{
				return;
			}

			float origin[3]{};
			float velocity[3]{};
			if (read_player_motion(origin, velocity))
			{
				game_console::set_live_status(format_motion(origin, velocity));
			}
			else
			{
				game_console::set_live_status("origin: unavailable velocity: unavailable");
			}
		}

		void origin_command(const command::params& params)
		{
			bool enabled = !internal_console_status_enabled.load(std::memory_order_relaxed);
			if (params.size() > 2)
			{
				game_console::set_live_status("usage: origin [0|1]");
				return;
			}

			if (params.size() == 2)
			{
				if (!_stricmp(params[1], "1") || !_stricmp(params[1], "on"))
				{
					enabled = true;
				}
				else if (!_stricmp(params[1], "0") || !_stricmp(params[1], "off"))
				{
					enabled = false;
				}
				else
				{
					game_console::set_live_status("usage: origin [0|1]");
					return;
				}
			}

			internal_console_status_enabled.store(enabled, std::memory_order_relaxed);
			if (enabled)
			{
				update_internal_console_status();
			}
			else
			{
				game_console::clear_live_status();
			}
		}
#endif

		void draw()
		{
			if (!dvars::cg_drawOrigin || !dvars::cg_drawOrigin->current.enabled)
			{
				return;
			}

			float origin[3]{};
			float velocity[3]{};
			if (!read_player_motion(origin, velocity))
			{
				return;
			}

			auto* const font = game::R_RegisterFont("fonts/consolefont");
			if (!font || font->pixelHeight <= 0)
			{
				return;
			}

			const auto placement = game::ScrPlace_GetViewPlacement();
			const auto right = placement.virtualViewableMax[0] > placement.virtualViewableMin[0]
				? placement.virtualViewableMax[0]
				: 640.0f;

			const auto* const debug_offset = game::Dvar_FindVar("cg_debugInfoCornerOffset");
			float y = debug_offset ? debug_offset->current.vector[1] : 0.0f;
			const auto line_height = static_cast<float>(font->pixelHeight) * native_line_spacing;

			y += line_height * static_cast<float>(fps_line_count());

			y += static_cast<float>(font->pixelHeight) * native_text_y_scale;
			const auto origin_text = utils::string::va("origin: (%.2f, %.2f, %.2f)", origin[0], origin[1], origin[2]);
			draw_right_aligned(origin_text, font, right, y);

			y += line_height;
			const auto velocity_text = utils::string::va("velocity: (%.2f, %.2f, %.2f)", velocity[0], velocity[1], velocity[2]);
			draw_right_aligned(velocity_text, font, right, y);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::loop(draw, scheduler::pipeline::renderer);
#ifdef DEBUG
			command::add("origin", origin_command);
			scheduler::loop(update_internal_console_status, scheduler::pipeline::main, console_update_interval);
#endif
		}

		void pre_destroy() override
		{
#ifdef DEBUG
			internal_console_status_enabled.store(false, std::memory_order_relaxed);
			game_console::clear_live_status();
#endif
		}
	};
}

REGISTER_COMPONENT(draw_origin::component)
