#pragma once
#include <cstdint>

namespace killcam_hud::policy
{
	constexpr bool flashbang_active(std::int32_t now, std::int32_t start,
		std::int32_t duration, int screen_blend)
	{
		// Match QoS Flashbanged menu expression 102C9EE0, including clock wrap.
		const auto remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(start)
			+ static_cast<std::uint32_t>(duration) - static_cast<std::uint32_t>(now));
		return remaining > 0 && screen_blend != 0;
	}
}
