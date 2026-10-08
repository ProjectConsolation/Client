#pragma once
namespace bots::targeting
{
	// Local policy inspired by KisakBlack sv_bot_mp.cpp's threat/sight
	// separation and close-target retention; no BO1 ABI structures imported.
	constexpr int visibility_lifetime_ms = 150;
	constexpr float perception_range = 2800.0f;
	struct sight_sample
	{
		bool valid{};
		int time{};
		float visibility{};
		float observer[3]{}, target[3]{};
	};
	inline float distance_squared(const float* a, const float* b)
	{
		float result = 0;
		for (int i = 0; i < 3; ++i) { const float d = a[i] - b[i]; result += d * d; }
		return result;
	}
	inline bool reusable(const sight_sample& s, int now, const float* observer, const float* target)
	{
		return s.valid && now >= s.time && now - s.time < visibility_lifetime_ms
			&& distance_squared(observer, s.observer) < 32.0f * 32.0f
			&& distance_squared(target, s.target) < 32.0f * 32.0f;
	}
	inline float threat_score(float visibility, float distance, bool current)
	{
		return visibility * 600000.0f - distance
			+ (current ? (distance < 384.0f * 384.0f ? 200000.0f : 60000.0f) : 0.0f);
	}
}
