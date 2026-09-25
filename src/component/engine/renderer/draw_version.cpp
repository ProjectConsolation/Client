#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

namespace draw_version
{
	namespace
	{
		// normalFont has a denser 19-pixel atlas than objectiveFont. Scaling it
		// to 0.75 retains the old 14-pixel watermark height without the blur.
		constexpr float watermark_font_scale = 0.75f;
		constexpr float version_font_scale = 0.85f;
		constexpr float watermark_margin_x = 8.0f;
		constexpr float watermark_margin_y = 6.0f;
		constexpr float shadow_offset_x = 1.0f;
		constexpr float shadow_offset_y = 1.0f;
		float shadow_color[4] = { 0.0f, 0.0f, 0.0f, 0.65f };
		float version_text_color[4] = { 0.20f, 0.55f, 1.0f, 0.85f };
		float watermark_text_color[4] = { 1.0f, 1.0f, 1.0f, 0.80f };
		const char* watermark_text = "Project: Consolation";
		std::atomic_bool overlay_enabled{false};
		float get_line_height(const game::Font_s* font, float scale)
		{
			if (!font)
			{
				return 18.0f * scale;
			}

			return static_cast<float>(font->pixelHeight) * scale;
		}

		float get_client_width()
		{
			RECT client_rect{};
			const auto window = *game::main_window;
			if (window && GetClientRect(window, &client_rect) && client_rect.right > client_rect.left)
			{
				return static_cast<float>(client_rect.right - client_rect.left);
			}

			return 640.0f;
		}

		float get_client_height()
		{
			RECT client_rect{};
			const auto window = *game::main_window;
			if (window && GetClientRect(window, &client_rect) && client_rect.bottom > client_rect.top)
			{
				return static_cast<float>(client_rect.bottom - client_rect.top);
			}

			return 480.0f;
		}

		float safe_area_fraction(const game::dvar_s* base, const game::dvar_s* adjusted)
		{
			const auto value = base ? base->current.value : (adjusted ? adjusted->current.value : 1.0f);
			return std::clamp(value, 0.0f, 1.0f);
		}

		float get_safe_right()
		{
			const auto width = get_client_width();
			const auto fraction = safe_area_fraction(
				dvars::safeArea_horizontal, dvars::safeArea_adjusted_horizontal);
			return width - width * (1.0f - fraction) * 0.5f;
		}

		float get_safe_top()
		{
			const auto height = get_client_height();
			const auto fraction = safe_area_fraction(
				dvars::safeArea_vertical, dvars::safeArea_adjusted_vertical);
			return height * (1.0f - fraction) * 0.5f;
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

		void draw_text_shadowed(const char* text, float x, float y, float scale,
			const game::Font_s* font, const float* color)
		{
			if (!text || !*text || !font || !color)
			{
				return;
			}

			game::R_AddCmdDrawText(text, 0x7FFFFFFF, const_cast<game::Font_s*>(font), x + shadow_offset_x, y + shadow_offset_y, scale, scale, 0.0f, shadow_color, 0);
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, const_cast<game::Font_s*>(font), x, y, scale, scale, 0.0f, color, 0);
		}

		void cg_draw_watermark()
		{
			if (!dvars::cg_drawWatermark || !dvars::cg_drawWatermark->current.enabled)
			{
				return;
			}

			const auto* const font = game::R_RegisterFont("fonts/normalFont");
			if (!font)
			{
				return;
			}

			const auto line_height = get_line_height(font, watermark_font_scale);
			const auto text_width = static_cast<float>(game::R_TextWidth(watermark_text,
				0x7FFFFFFF, const_cast<game::Font_s*>(font))) * watermark_font_scale;
			const auto x = std::max(1.0f, get_safe_right() - text_width - watermark_margin_x);
			const auto y = get_safe_top() + watermark_margin_y + line_height;
			game::R_AddCmdDrawText(watermark_text, 0x7FFFFFFF,
				const_cast<game::Font_s*>(font), x, y, watermark_font_scale,
				watermark_font_scale, 0.0f, watermark_text_color, 0);
		}

		void cg_draw_version()
		{
			if (!dvars::cg_drawVersion || !dvars::cg_drawVersion->current.enabled)
			{
				return;
			}

			const auto* const font = game::R_RegisterFont("fonts/normalFont");
			if (!font)
			{
				return;
			}

			const auto* const version_buffer_ptr = get_version_text();
			if (!version_buffer_ptr || !*version_buffer_ptr)
			{
				return;
			}

			// The local ScreenPlacement declaration does not match this QoS build.
			// Use the actual client area so the saved right-edge margin cannot be
			// clamped against the misread 480-pixel viewport field.
			const auto text_width = static_cast<float>(game::R_TextWidth(version_buffer_ptr,
				std::numeric_limits<int>::max(), const_cast<game::Font_s*>(font))) * version_font_scale;
			const auto x_offset = dvars::cg_drawVersionX ? dvars::cg_drawVersionX->current.value : 50.0f;
			const auto y_offset = dvars::cg_drawVersionY ? dvars::cg_drawVersionY->current.value : 18.0f;
			const auto x = std::max(1.0f, get_safe_right() - text_width - x_offset);
			const auto y = get_safe_top() + watermark_margin_y
				+ get_line_height(font, watermark_font_scale)
				+ y_offset + get_line_height(font, version_font_scale);

			draw_text_shadowed(version_buffer_ptr, x, y, version_font_scale, font, version_text_color);
		}

	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			overlay_enabled.store(true, std::memory_order_release);
			scheduler::loop([]()
				{
					if (!overlay_enabled.load(std::memory_order_acquire))
					{
						return;
					}

					cg_draw_watermark();
					cg_draw_version();
				}, scheduler::pipeline::renderer);
		}

		void pre_destroy() override
		{
			overlay_enabled.store(false, std::memory_order_release);
		}
	};
}

REGISTER_COMPONENT(draw_version::component)
