#pragma once

#include "game/structs.hpp"

namespace gsc
{
	void add_function(const char* name, game::qos::function_t func, int type = 0);
	void add_method(const char* name, game::qos::method_t func, int type = 0);
}
