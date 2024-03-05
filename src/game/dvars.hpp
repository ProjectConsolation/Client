#pragma once

#include "game.hpp"
#include "structs.hpp"
#include <string>
#include <unordered_map>

namespace dvars
{
	struct dvar_info
	{
		std::string name;
		std::string description;
	};

	extern std::unordered_map<std::int32_t, dvar_info> dvar_map;

	extern game::dvar_s* con_inputBoxColor;
	extern game::dvar_s* con_inputHintBoxColor;
	extern game::dvar_s* con_outputBarColor;
	extern game::dvar_s* con_outputSliderColor;
	extern game::dvar_s* con_outputWindowColor;
	extern game::dvar_s* con_inputDvarMatchColor;
	extern game::dvar_s* con_inputDvarValueColor;
	extern game::dvar_s* con_inputDvarInactiveValueColor;
	extern game::dvar_s* con_inputCmdMatchColor;

	/*
	extern game::dvar_s* r_fullscreen;
	extern game::dvar_s* r_borderless;
	*/

	std::string dvar_get_vector_domain(const int components, const game::DvarLimits& domain);
	std::string dvar_get_domain(const game::dvar_type type, const game::DvarLimits& domain);

	game::dvar_s* Dvar_RegisterVec4(const char* dvar_name, const char* description, float x, float y, float z, float w, float min_value, float max_value, std::uint16_t flags);
	game::dvar_s* Dvar_RegisterBool(const char* dvar_name, const char* description, int min_value, int max_value, std::uint16_t flags);
	char* Dvar_ValueToString(game::dvar_s* dvar, game::DvarValue value);
}