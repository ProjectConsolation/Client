#pragma once

#include "game/game.hpp"

namespace scaleform
{
	game::RawFile* try_override(const char* name);
	void clear_overrides();
}
