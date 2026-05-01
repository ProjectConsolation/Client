#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>

namespace ultrawide
{
	namespace
	{
		constexpr auto aspect_ratio_scalar = 0x1127BAEC;
		constexpr auto custom_mode_fullscreen_gate = 0x103BE1C9;
		constexpr auto renderer_aspect_ratio = 0x10E271AC;
		constexpr auto renderer_monitor_width = 0x10E27190;
		constexpr auto renderer_monitor_height = 0x10E27194;
		constexpr auto renderer_render_width = 0x10E27198;
		constexpr auto renderer_render_height = 0x10E2719C;
		constexpr auto renderer_monitor_aspect = 0x10E271B0;
		constexpr auto renderer_render_aspect = 0x10E271B4;

		bool sticky_custom_resolution_enabled = false;
		float sticky_custom_ratio = 16.0f / 9.0f;
		float original_aspect_ratio = 16.0f / 9.0f;
		float original_renderer_aspect_ratio = 16.0f / 9.0f;
		float original_renderer_monitor_aspect = 0.0f;
		float original_renderer_render_aspect = 0.0f;
		bool original_aspect_ratio_captured = false;

		bool parse_resolution(const std::string& text, int& width, int& height)
		{
			width = 0;
			height = 0;

			return std::sscanf(text.c_str(), "%ix%i", &width, &height) == 2
				&& width > 0
				&& height > 0;
		}

		void sync_sticky_custom_resolution_state()
		{
			if (!dvars::r_ultrawideCustomMode || !dvars::r_ultrawideCustomMode->current.string)
			{
				return;
			}

			int width = 0;
			int height = 0;
			if (parse_resolution(dvars::r_ultrawideCustomMode->current.string, width, height))
			{
				sticky_custom_resolution_enabled = true;
				sticky_custom_ratio = static_cast<float>(width) / static_cast<float>(height);
			}
			else
			{
				sticky_custom_resolution_enabled = false;
				sticky_custom_ratio = 16.0f / 9.0f;
			}
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

		void set_float_dvar(game::dvar_s* dvar, const float value)
		{
			if (!dvar)
			{
				return;
			}

			dvar->current.value = value;
			dvar->latched.value = value;
			dvar->reset.value = value;
		}

		void set_custom_resolution(const std::string& resolution)
		{
			int width = 0;
			int height = 0;
			if (!parse_resolution(resolution, width, height))
			{
				console::info("usage: setcustomres <width>x<height>\n");
				return;
			}

			const auto ratio = static_cast<float>(width) / static_cast<float>(height);
			sticky_custom_resolution_enabled = true;
			sticky_custom_ratio = ratio;
			command::execute(std::format("seta r_customMode \"{}\"\n", resolution));
			command::execute(std::format("seta r_ultrawideCustomMode \"{}\"\n", resolution));
			command::execute("seta r_aspectRatioCustomEnable 1\n");
			command::execute(std::format("seta r_aspectRatioCustom {:.6f}\n", ratio));
			command::execute("vid_restart\n");

			console::info("setcustomres: queued %s using current window mode dvars (aspect %.6f)\n", resolution.c_str(), ratio);
		}

		void clear_custom_resolution()
		{
			sticky_custom_resolution_enabled = false;
			sticky_custom_ratio = 16.0f / 9.0f;
			command::execute("seta r_customMode disabled\n");
			command::execute("seta r_ultrawideCustomMode disabled\n");
			command::execute("seta r_aspectRatioCustomEnable 0\n");
			command::execute("vid_restart\n");

			console::info("setcustomres: cleared custom resolution override\n");
		}

		void apply_custom_aspect_ratio()
		{
			auto* const aspect_ratio = reinterpret_cast<float*>(game::game_offset(aspect_ratio_scalar));
			auto* const renderer_ratio = reinterpret_cast<float*>(game::game_offset(renderer_aspect_ratio));
			auto* const monitor_aspect = reinterpret_cast<float*>(game::game_offset(renderer_monitor_aspect));
			auto* const render_aspect = reinterpret_cast<float*>(game::game_offset(renderer_render_aspect));
			auto* const monitor_width = reinterpret_cast<int*>(game::game_offset(renderer_monitor_width));
			auto* const monitor_height = reinterpret_cast<int*>(game::game_offset(renderer_monitor_height));
			auto* const render_width = reinterpret_cast<int*>(game::game_offset(renderer_render_width));
			auto* const render_height = reinterpret_cast<int*>(game::game_offset(renderer_render_height));
			if (!aspect_ratio)
			{
				return;
			}

			if (!original_aspect_ratio_captured)
			{
				original_aspect_ratio = *aspect_ratio;
				if (renderer_ratio) original_renderer_aspect_ratio = *renderer_ratio;
				if (monitor_aspect) original_renderer_monitor_aspect = *monitor_aspect;
				if (render_aspect) original_renderer_render_aspect = *render_aspect;
				original_aspect_ratio_captured = true;
			}

			sync_sticky_custom_resolution_state();

			if (sticky_custom_resolution_enabled)
			{
				set_bool_dvar(dvars::r_aspectRatioCustomEnable, true);
				set_float_dvar(dvars::r_aspectRatioCustom, sticky_custom_ratio);
			}

			const auto enabled = dvars::r_aspectRatioCustomEnable && dvars::r_aspectRatioCustomEnable->current.enabled;
			const auto custom_ratio = dvars::r_aspectRatioCustom
				? std::clamp(dvars::r_aspectRatioCustom->current.value, 4.0f / 3.0f, 63.0f / 9.0f)
				: original_aspect_ratio;

			*aspect_ratio = original_aspect_ratio;
			if (renderer_ratio)
			{
				*renderer_ratio = enabled ? custom_ratio : original_renderer_aspect_ratio;
			}

			if (enabled)
			{
				if (monitor_aspect && monitor_width && monitor_height && *monitor_width > 0)
				{
					*monitor_aspect = static_cast<float>(*monitor_height) * custom_ratio / static_cast<float>(*monitor_width);
				}

				if (render_aspect && render_width && render_height && *render_width > 0)
				{
					*render_aspect = custom_ratio * static_cast<float>(*render_height) / static_cast<float>(*render_width);
				}
			}
			else
			{
				if (monitor_aspect) *monitor_aspect = original_renderer_monitor_aspect;
				if (render_aspect) *render_aspect = original_renderer_render_aspect;
			}

			if (auto* const widescreen = game::Dvar_FindVar("wideScreen"))
			{
				const auto is_wide = enabled
					? (custom_ratio > 1.5f)
					: (original_renderer_aspect_ratio > 1.5f);
				set_bool_dvar(widescreen, is_wide);
			}

			if (auto* const is_widescreen = game::Dvar_FindVar("r_isWideScreen"))
			{
				const auto is_wide = enabled
					? (custom_ratio > 1.5f)
					: (original_renderer_aspect_ratio > 1.5f);
				set_bool_dvar(is_widescreen, is_wide);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::once([]()
			{
				dvars::r_aspectRatioCustomEnable = dvars::Dvar_RegisterBool(
					"r_aspectRatioCustomEnable", 0,
					"Enable custom ultrawide aspect ratio overrides.",
					game::dvar_flags::saved);

				dvars::r_aspectRatioCustom = dvars::Dvar_RegisterFloat(
					"r_aspectRatioCustom",
					"Screen aspect ratio override. Divide width by height to get the aspect ratio value. Example: 21 / 9 = 2.3333",
					16.0f / 9.0f, 4.0f / 3.0f, 63.0f / 9.0f,
					game::dvar_flags::saved);

				dvars::r_ultrawideCustomMode = dvars::Dvar_RegisterString(
					"r_ultrawideCustomMode",
					"disabled",
					"Saved ultrawide custom resolution in WxH format for Consolation.",
					game::dvar_flags::saved);

				sticky_custom_resolution_enabled = dvars::r_aspectRatioCustomEnable && dvars::r_aspectRatioCustomEnable->current.enabled;
				if (dvars::r_aspectRatioCustom)
				{
					sticky_custom_ratio = std::clamp(dvars::r_aspectRatioCustom->current.value, 4.0f / 3.0f, 63.0f / 9.0f);
				}
				sync_sticky_custom_resolution_state();

				command::add("setcustomres", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("usage: setcustomres <width>x<height>\n");
						return;
					}

					set_custom_resolution(params[1]);
				});

				command::add("clearcustomres", []()
				{
					clear_custom_resolution();
				});

				command::add("dumpultrawide", []()
				{
					const auto* const cl_ingame = game::Dvar_FindVar("cl_ingame");
					const auto* const widescreen = game::Dvar_FindVar("wideScreen");
					const auto* const is_widescreen = game::Dvar_FindVar("r_isWideScreen");
					const auto* const aspect_ratio = reinterpret_cast<float*>(game::game_offset(aspect_ratio_scalar));
					const auto* const renderer_ratio = reinterpret_cast<float*>(game::game_offset(renderer_aspect_ratio));
					const auto* const monitor_aspect = reinterpret_cast<float*>(game::game_offset(renderer_monitor_aspect));
					const auto* const render_aspect = reinterpret_cast<float*>(game::game_offset(renderer_render_aspect));

					console::info(
						"dumpultrawide: enabled=%d custom=%.6f cl_ingame=%d keyCatchers=0x%X aspect=%.6f renderer=%.6f monitor=%.6f render=%.6f wideScreen=%d r_isWideScreen=%d\n",
						dvars::r_aspectRatioCustomEnable ? static_cast<int>(dvars::r_aspectRatioCustomEnable->current.enabled) : -1,
						dvars::r_aspectRatioCustom ? dvars::r_aspectRatioCustom->current.value : -1.0f,
						cl_ingame ? static_cast<int>(cl_ingame->current.enabled) : -1,
						game::keyCatchers ? *game::keyCatchers : 0,
						aspect_ratio ? *aspect_ratio : -1.0f,
						renderer_ratio ? *renderer_ratio : -1.0f,
						monitor_aspect ? *monitor_aspect : -1.0f,
						render_aspect ? *render_aspect : -1.0f,
						widescreen ? static_cast<int>(widescreen->current.enabled) : -1,
						is_widescreen ? static_cast<int>(is_widescreen->current.enabled) : -1);
				});
			}, scheduler::main);
		}

		void pre_destroy() override
		{
			dvars::r_aspectRatioCustomEnable = nullptr;
			dvars::r_aspectRatioCustom = nullptr;
			dvars::r_ultrawideCustomMode = nullptr;
		}
	};
}

REGISTER_COMPONENT(ultrawide::component)
