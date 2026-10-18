#pragma once

#include <array>
#include <string_view>

namespace menu_video_tint
{
	// Match the launcher's original wine accent, #952F48 (App.xaml), rather
	// than the previous bright scarlet. Scale decoded luminance, not video
	// alpha: the moving flag and native transition fades must remain visible.
	inline constexpr unsigned int wine_rgb = 0x952F48;
	inline constexpr float red[3]{149.0f / 255.0f, 47.0f / 255.0f, 72.0f / 255.0f};

	constexpr std::array<float, 4> background_color(std::array<float, 4> rgba,
		const bool enabled, const bool in_game, const std::string_view movie, const bool cinematic_material)
	{
		// The visible native cinematic is separate from Scaleform's offscreen
		// Bink copy. Grade only the frontend flag, never arbitrary cinematics.
		if (enabled && !in_game && cinematic_material && movie == "bink_bkg_loop_1")
			for (unsigned channel = 0; channel < 3; ++channel) rgba[channel] *= red[channel];
		return rgba;
	}

	// QoS PC 10412B20 uploads native YUV->RGB rows c0..c2 and c3 constants for
	// the bink_bkg_loop_1 shader compiled in 10413510 (p.rgb = dot(row, yuv1)).
	// Tint the decoded background only, leaving the c3 alpha/constant row and
	// the native matrix storage intact.
	constexpr std::array<float, 16> constants(std::array<float, 16> values, const bool tint)
	{
		if (tint)
		{
			// Convert decoded video to luminance first: a blue source must not
			// survive as purple. Output red, independently of source hue.
			const auto native = values;
			for (unsigned row = 0; row < 3; ++row)
				for (unsigned column = 0; column < 4; ++column)
					values[row * 4 + column] = red[row] * (native[column] * 0.2126f
						+ native[4 + column] * 0.7152f + native[8 + column] * 0.0722f);
		}
		return values;
	}
}
