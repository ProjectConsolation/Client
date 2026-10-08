#pragma once
#include <cmath>

namespace bots::combat
{
	inline int angle_units(float degrees)
	{
		return static_cast<int>(std::remainder(degrees, 360.0f) * (65536.0f / 360.0f));
	}
	inline int command_angle(int desired, float delta_degrees)
	{
		return (desired - angle_units(delta_degrees)) & 65535;
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
	inline bool attack(fire_control& state, int now, bool allowed)
	{
		if (!allowed) { state = {}; return false; }
		if (!state.active || now < state.start) state = {true, now};
		// Adapt KisakBlack Bot_UpdateWeapon's timed fire/release principle,
		// not its BO1 weapon ABI. Releases allow semi-auto weapons to retrigger.
		return (now - state.start) % 350 < 250;
	}
}
