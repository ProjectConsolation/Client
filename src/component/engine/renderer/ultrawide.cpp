#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"
#include <utils/hook.hpp>
#include <charconv>
#include <cmath>

namespace ultrawide
{
	namespace
	{
		std::uintptr_t store_window_settings_address{};
		bool parse_resolution(const std::string& text, int& width, int& height)
		{
			width = height = 0;
			const auto separator = text.find('x');
			if (separator == std::string::npos) return false;
			const auto* first = text.data();
			const auto* middle = first + separator;
			const auto* last = first + text.size();
			const auto parsed_width = std::from_chars(first, middle, width);
			const auto parsed_height = std::from_chars(middle + 1, last, height);
			return parsed_width.ec == std::errc{} && parsed_width.ptr == middle
				&& parsed_height.ec == std::errc{} && parsed_height.ptr == last
				&& width > 0 && width <= 16384 && height > 0 && height <= 16384
				&& static_cast<float>(width) / height >= 4.0f / 3.0f
				&& static_cast<float>(width) / height <= 63.0f / 9.0f;
		}
		void set_custom_resolution(const std::string& resolution)
		{
			int width{}, height{};
			if (!parse_resolution(resolution, width, height))
			{
				console::info("usage: setcustomres <width>x<height> (aspect 4:3 to 63:9, dimensions 1..16384)\n");
				return;
			}
			const auto ratio = static_cast<float>(width) / height;
			const auto canonical = std::format("{}x{}", width, height);
			command::execute(std::format("seta r_customMode \"{}\"\n", canonical));
			command::execute(std::format("seta r_ultrawideCustomMode \"{}\"\n", canonical));
			command::execute("seta r_aspectRatioCustomEnable 1\n");
			command::execute(std::format("seta r_aspectRatioCustom {:.6f}\n", ratio));
			command::execute("vid_restart\n");
			console::info("setcustomres: queued %s using current window mode (aspect %.6f)\n", canonical.c_str(), ratio);
		}
		void clear_custom_resolution()
		{
			command::execute("seta r_customMode disabled\n");
			command::execute("seta r_ultrawideCustomMode disabled\n");
			command::execute("seta r_aspectRatioCustomEnable 0\n");
			command::execute("vid_restart\n");
		}
		void apply_custom_aspect_ratio()
		{
			// QoS PC R_StoreWindowSettings (103BD100), compared with KisakCOD
			// r_init.cpp: display dimensions +0/+4, scene dimensions +8/+12,
			// window aspect +28, display pixel aspect +32, scene pixel aspect +36.
			// Never write 1127BAEC: it belongs to console/input state, not vidConfig.
			const auto* dimensions = reinterpret_cast<const unsigned*>(game::game_offset(0x10E27190));
			auto* aspect = reinterpret_cast<float*>(game::game_offset(0x10E271AC));
			if (!dimensions[0] || !dimensions[1] || !dimensions[2] || !dimensions[3]) return;
			const auto enabled = dvars::r_aspectRatioCustomEnable && dvars::r_aspectRatioCustomEnable->current.enabled;
			const auto* native_aspect = game::Dvar_FindVar("r_aspectRatio");
			const auto automatic = static_cast<float>(dimensions[0]) / dimensions[1];
			// Native Auto snaps to at most 16:9. Extend only wider displays;
			// preserve explicit native aspect choices and ordinary resolutions.
			if (!enabled && (!native_aspect || native_aspect->current.integer != 0 || automatic <= 16.0f / 9.0f)) return;
			const auto ratio = enabled && dvars::r_aspectRatioCustom
				? std::clamp(dvars::r_aspectRatioCustom->current.value, 4.0f / 3.0f, 63.0f / 9.0f) : automatic;
			if (!std::isfinite(ratio)) return;
			aspect[0] = ratio;
			aspect[1] = static_cast<float>(dimensions[1]) * ratio / dimensions[0];
			aspect[2] = static_cast<float>(dimensions[3]) * ratio / dimensions[2];
			if (auto* wide = game::Dvar_FindVar("wideScreen"))
			{
				wide->current.enabled = ratio > 4.0f / 3.0f;
				wide->latched.enabled = wide->current.enabled;
			}
		}
		__declspec(naked) void store_window_settings_stub()
		{
			// Verified init/device-reset callers pass GfxWindowParms in EAX.
			// Preserve native result/registers before viewport/FOV setup.
			__asm
			{
				call store_window_settings_address
				pushfd
				pushad
				call apply_custom_aspect_ratio
				popad
				popfd
				ret
			}
		}
	}
	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			store_window_settings_address = game::game_offset(0x103BD100);
			utils::hook::call(game::game_offset(0x103BEDB2), store_window_settings_stub);
			utils::hook::call(game::game_offset(0x103BEE1D), store_window_settings_stub);
			scheduler::once([]
			{
				dvars::r_aspectRatioCustomEnable = dvars::Dvar_RegisterBool("r_aspectRatioCustomEnable", 0,
					"Enable custom aspect ratio; apply with vid_restart.", game::dvar_flags::saved);
				dvars::r_aspectRatioCustom = dvars::Dvar_RegisterFloat("r_aspectRatioCustom",
					"Screen width divided by height; apply with vid_restart.",
					16.0f / 9.0f, 4.0f / 3.0f, 63.0f / 9.0f, game::dvar_flags::saved);
				dvars::r_ultrawideCustomMode = dvars::Dvar_RegisterString("r_ultrawideCustomMode", "disabled",
					"Saved custom resolution in WxH format.", game::dvar_flags::saved);
				command::add("setcustomres", [](const command::params& params)
				{
					set_custom_resolution(params.size() == 2 ? params[1] : "");
				});
				command::add("clearcustomres", clear_custom_resolution);
				command::add("dumpultrawide", []
				{
					const auto* dimensions = reinterpret_cast<const unsigned*>(game::game_offset(0x10E27190));
					const auto* aspect = reinterpret_cast<const float*>(game::game_offset(0x10E271AC));
					console::info("dumpultrawide: enabled=%d custom=%.6f display=%ux%u scene=%ux%u window=%.6f displayPixel=%.6f scenePixel=%.6f\n",
						dvars::r_aspectRatioCustomEnable->current.enabled, dvars::r_aspectRatioCustom->current.value,
						dimensions[0], dimensions[1], dimensions[2], dimensions[3], aspect[0], aspect[1], aspect[2]);
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
