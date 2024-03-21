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
	extern game::dvar_s* g_debugVelocity;

	std::string dvar_get_vector_domain(const int components, const game::DvarLimits& domain);
	std::string dvar_get_domain(const game::dvar_type type, const game::DvarLimits& domain);

	game::dvar_s* Dvar_RegisterFloat(const char* dvar_name, const char* description, float float_value, float min_value, float max_value, std::uint16_t flags);
	game::dvar_s* Dvar_RegisterVec4(const char* dvar_name, const char* description, float x, float y, float z, float w, float min_value, float max_value, std::uint16_t flags);
	game::dvar_s* Dvar_RegisterBool(const char* dvar_name, int value_default, const char* description, std::uint16_t flags);

	char* Dvar_ValueToString(game::dvar_s* dvar, game::DvarValue value);

	namespace overrides
	{
		struct dvar_base
		{
			unsigned int flags{};
		};

		struct dvar_bool : dvar_base
		{
			bool value{};
		};

		struct dvar_float : dvar_base
		{
			float value{};
			float min{};
			float max{};
		};

		struct dvar_vector2 : dvar_base
		{
			float x{};
			float y{};
			float min{};
			float max{};
		};

		struct dvar_vector3 : dvar_base
		{
			float x{};
			float y{};
			float z{};
			float min{};
			float max{};
		};

		struct dvar_enum : dvar_base
		{
			const char* const* value_list{};
			int default_index{};
		};

		struct dvar_int : dvar_base
		{
			int value{};
			int min{};
			int max{};
		};

		struct dvar_string : dvar_base
		{
			std::string value{};
		};

		extern std::unordered_map<std::string, dvar_bool> register_bool_overrides;
		extern std::unordered_map<std::string, dvar_int> register_int_overrides;
		extern std::unordered_map<std::string, dvar_float> register_float_overrides;
		void register_bool(const std::string& name, const bool value, const unsigned int flags);
		void register_int(const std::string& name, const int value, const int min, const int max, const unsigned int flags);
		void register_float(const std::string& name, const float value, const float min, const float max, const unsigned int flags);
	}
}