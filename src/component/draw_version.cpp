#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

namespace draw_version
{
	namespace
	{
		constexpr float watermark_font_scale = 0.25f;
		constexpr float version_font_scale = 1.0f;
		constexpr float watermark_margin_x = 8.0f;
		constexpr float watermark_margin_y = 6.0f;
		constexpr float shadow_offset_x = 1.0f;
		constexpr float shadow_offset_y = 1.0f;
		float shadow_color[4] = { 0.0f, 0.0f, 0.0f, 0.65f };
		float text_color[4] = { 0.86f, 0.82f, 0.72f, 0.60f };
		const char* watermark_text = "Project: Consolation";

		float resolve_layout_width(const game::ScreenPlacement& scr_place)
		{
			const auto real_a = scr_place.realViewportSize[0];
			const auto real_b = scr_place.realViewportSize[1];
			const auto virtual_a = scr_place.virtualViewableMax[0];
			const auto virtual_b = scr_place.virtualViewableMax[1];

			if (real_a > 0.0f && real_b > 0.0f)
			{
				return std::max(real_a, real_b);
			}

			if (virtual_a > 0.0f && virtual_b > 0.0f)
			{
				return std::max(virtual_a, virtual_b);
			}

			return 640.0f;
		}

		float resolve_layout_height(const game::ScreenPlacement& scr_place)
		{
			const auto real_a = scr_place.realViewportSize[0];
			const auto real_b = scr_place.realViewportSize[1];
			const auto virtual_a = scr_place.virtualViewableMax[0];
			const auto virtual_b = scr_place.virtualViewableMax[1];

			if (real_a > 0.0f && real_b > 0.0f)
			{
				return std::min(real_a, real_b);
			}

			if (virtual_a > 0.0f && virtual_b > 0.0f)
			{
				return std::min(virtual_a, virtual_b);
			}

			return 480.0f;
		}

		float get_layout_width()
		{
			return resolve_layout_width(game::ScrPlace_GetViewPlacement());
		}

		float get_layout_height()
		{
			return resolve_layout_height(game::ScrPlace_GetViewPlacement());
		}

		float get_text_width(const char* text, const game::Font_s* font, float scale)
		{
			if (!text || !*text || !font)
			{
				return 0.0f;
			}

			return static_cast<float>(game::R_TextWidth(text, 0x7FFFFFFF, const_cast<game::Font_s*>(font))) * scale;
		}

		float get_line_height(const game::Font_s* font, float scale)
		{
			if (!font)
			{
				return 18.0f * scale;
			}

			return static_cast<float>(font->pixelHeight) * scale;
		}

		const char* get_version_text()
		{
			const auto* const version = game::Dvar_FindVar("version");
			if (version && version->current.string && *version->current.string)
			{
				return version->current.string;
			}

			return "";
		}

		void draw_text_shadowed(const char* text, float x, float y, float scale, const game::Font_s* font)
		{
			if (!text || !*text || !font)
			{
				return;
			}

			game::R_AddCmdDrawText(text, 0x7FFFFFFF, const_cast<game::Font_s*>(font), x + shadow_offset_x, y + shadow_offset_y, scale, scale, 0.0f, shadow_color, 0);
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, const_cast<game::Font_s*>(font), x, y, scale, scale, 0.0f, text_color, 0);
		}

		void draw_bottom_right_text(const char* text, float baseline_y, float scale, const game::Font_s* font)
		{
			if (!text || !*text || !font)
			{
				return;
			}

			const auto screen_width = get_layout_width();
			const auto text_width = get_text_width(text, font, scale);
			const auto x = std::max(1.0f, screen_width - text_width - watermark_margin_x);
			draw_text_shadowed(text, x, baseline_y, scale, font);
		}

		void cg_draw_watermark()
		{
			if (!dvars::cg_drawWatermark || !dvars::cg_drawWatermark->current.enabled)
			{
				return;
			}

			const auto* const font = game::R_RegisterFont("fonts/objectivefont");
			if (!font)
			{
				return;
			}

			const auto screen_height = get_layout_height();
			const auto line_height = get_line_height(font, watermark_font_scale);
			const auto y = std::max(line_height, screen_height - watermark_margin_y);
			draw_bottom_right_text(watermark_text, y, watermark_font_scale, font);
		}

		void cg_draw_version()
		{
			if (!dvars::cg_drawVersion || !dvars::cg_drawVersion->current.enabled)
			{
				return;
			}

			const auto* const font = game::R_RegisterFont("fonts/consolefont");
			if (!font)
			{
				return;
			}

			const auto scr_place = game::ScrPlace_GetViewPlacement();
			const auto viewport_width = scr_place.realViewportSize[0] > 0.0f ? scr_place.realViewportSize[0] : get_layout_width();
			if (viewport_width <= 0.0f)
			{
				return;
			}

			const auto* const version_buffer_ptr = get_version_text();
			if (!version_buffer_ptr || !*version_buffer_ptr)
			{
				return;
			}

			const auto text_width = static_cast<float>(game::R_TextWidth(version_buffer_ptr, std::numeric_limits<int>::max(), const_cast<game::Font_s*>(font)));
			const auto x_offset = dvars::cg_drawVersionX ? dvars::cg_drawVersionX->current.value : 50.0f;
			const auto y_offset = dvars::cg_drawVersionY ? dvars::cg_drawVersionY->current.value : 18.0f;
			const auto x = x_offset + viewport_width - text_width;
			const auto y = y_offset + static_cast<float>(font->pixelHeight);

			draw_text_shadowed(version_buffer_ptr, x, y, version_font_scale, font);
		}

	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::loop([]()
				{
					cg_draw_watermark();
					cg_draw_version();
				}, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(draw_version::component)
