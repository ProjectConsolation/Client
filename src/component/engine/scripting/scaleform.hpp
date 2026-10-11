#pragma once

#include "game/game.hpp"
#include <string>

namespace scaleform
{
	std::string bundled_path(const char* name);
	game::RawFile* try_override(const char* name);
	void clear_overrides();
}
