#pragma once
#include <cmath>
#include <algorithm>

namespace bots::combat
{
	struct difficulty_settings
	{
		int turn_degrees, reaction_ms, burst_ms, cycle_ms;
		float aim_error_degrees;
		bool ads;
	};
	inline difficulty_settings difficulty(int level)
	{
		constexpr difficulty_settings levels[] = {
			{45, 900, 100, 1400, 4.0f, false},
			{75, 650, 130, 1000, 2.5f, false},
			{100, 450, 160, 800, 1.25f, true},
			{120, 350, 180, 650, 0.5f, true}
		};
		return levels[std::clamp(level, 0, 3)];
	}
	inline int angle_units(float degrees)
	{
		return static_cast<int>(std::remainder(degrees, 360.0f) * (65536.0f / 360.0f));
	}
	inline int command_angle(int desired, float delta_degrees)
	{
		return (desired - angle_units(delta_degrees)) & 65535;
	}
	inline int turn_angle(int current, int desired, int elapsed_ms, int degrees_per_second = 120)
	{
		const int delta = ((desired - current + 32768) & 65535) - 32768;
		const int limit = static_cast<int>(std::clamp(elapsed_ms, 0, 100) *
			(degrees_per_second * (65536.0 / 360.0)) / 1000.0);
		return (current + std::clamp(delta, -limit, limit)) & 65535;
	}
	inline void cap_health(int& entity, int& stats, int maximum)
	{
		// Never heal wounds or resurrect a lethal hit while snapshots catch up.
		if (entity > maximum) entity = maximum;
		if (stats > maximum) stats = maximum;
	}
	// Timed native-command attack state; independent of health lifecycle.
	struct fire_control
	{
		bool active{};
		int start{};
	};
	struct life_health
	{
		bool initialized{};
		int spawn{};
		int maximum{100};
	};
	inline bool begin_life(life_health& life, int spawn, int requested, int entity, int stats)
	{
		if (entity <= 0 || stats <= 0) return false;
		if (life.initialized && life.spawn == spawn) return false;
		life = {true, spawn, requested};
		return true;
	}
	inline bool attack(fire_control& state, int now, bool allowed,
		difficulty_settings settings = difficulty(3))
	{
		if (!allowed) { state = {}; return false; }
		if (!state.active || now < state.start) state = {true, now};
		// Adapt KisakBlack Bot_UpdateWeapon's timed fire/release principle,
		// not its BO1 weapon ABI. Releases allow semi-auto weapons to retrigger.
		const int elapsed = now - state.start;
		if (elapsed < settings.reaction_ms) return false;
		return (elapsed - settings.reaction_ms) % settings.cycle_ms < settings.burst_ms;
	}
}
