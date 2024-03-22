#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include <component/scheduler.hpp>

#include <utils/string.hpp>

#include "game.hpp"
#include "dvars.hpp"
#include <component/console.hpp>
#include <utils/hook.hpp>

namespace dvars
{
	std::unordered_map<std::int32_t, dvar_info> dvar_map;

	game::dvar_s* con_inputBoxColor = nullptr;
	game::dvar_s* con_inputHintBoxColor = nullptr;
	game::dvar_s* con_outputBarColor = nullptr;
	game::dvar_s* con_outputSliderColor = nullptr;
	game::dvar_s* con_outputWindowColor = nullptr;
	game::dvar_s* con_inputDvarMatchColor = nullptr;
	game::dvar_s* con_inputDvarValueColor = nullptr;
	game::dvar_s* con_inputDvarInactiveValueColor = nullptr;
	game::dvar_s* con_inputCmdMatchColor = nullptr;
	game::dvar_s* g_debugVelocity = nullptr;
	game::dvar_s* g_debugLocalization = nullptr;
	game::dvar_s* r_borderless = nullptr;

	// TODO: remake: cg_drawVersion, cg_overheadNamesFont

	std::string dvar_get_vector_domain(const int components, const game::DvarLimits& domain)
	{
		if (domain.vector.min == -FLT_MAX)
		{
			if (domain.vector.max == FLT_MAX)
			{
				return utils::string::va("Domain is any %iD vector", components);
			}
			else
			{
				return utils::string::va("Domain is any %iD vector with components %g or smaller", components,
					domain.vector.max);
			}
		}
		else if (domain.vector.max == FLT_MAX)
		{
			return utils::string::va("Domain is any %iD vector with components %g or bigger", components,
				domain.vector.min);
		}
		else
		{
			return utils::string::va("Domain is any %iD vector with components from %g to %g", components,
				domain.vector.min, domain.vector.max);
		}
	}

	std::string dvar_get_domain(const game::dvar_type type, const game::DvarLimits& domain)
	{
		std::string str;

		switch (type)
		{
		case game::dvar_type::boolean:
			return "Domain is 0 or 1"s;

		case game::dvar_type::value:
			if (domain.value.min == -FLT_MAX)
			{
				if (domain.value.max == FLT_MAX)
				{
					return "Domain is any number"s;
				}
				else
				{
					return utils::string::va("Domain is any number %g or smaller", domain.value.max);
				}
			}
			else if (domain.value.max == FLT_MAX)
			{
				return utils::string::va("Domain is any number %g or bigger", domain.value.min);
			}
			else
			{
				return utils::string::va("Domain is any number from %g to %g", domain.value.min, domain.value.max);
			}

		case game::dvar_type::vec2:
			return dvar_get_vector_domain(2, domain);

		case game::dvar_type::rgb:
		case game::dvar_type::vec3:
			return dvar_get_vector_domain(3, domain);

		case game::dvar_type::vec4:
			return dvar_get_vector_domain(4, domain);

		case game::dvar_type::integer:
			if (domain.enumeration.stringCount == INT_MIN)
			{
				if (domain.integer.max == INT_MAX)
				{
					return "Domain is any integer"s;
				}
				else
				{
					return utils::string::va("Domain is any integer %i or smaller", domain.integer.max);
				}
			}
			else if (domain.integer.max == INT_MAX)
			{
				return utils::string::va("Domain is any integer %i or bigger", domain.integer.min);
			}
			else
			{
				return utils::string::va("Domain is any integer from %i to %i", domain.integer.min, domain.integer.max);
			}

		case game::dvar_type::color:
			return "Domain is any 4-component color, in RGBA format"s;

		case game::dvar_type::enumeration:
			str = "Domain is one of the following:"s;

			for (auto string_index = 0; string_index < domain.enumeration.stringCount; ++string_index)
			{
				str += utils::string::va("\n  %2i: %s", string_index, domain.enumeration.strings[string_index]);
			}

			return str;

		case game::dvar_type::string:
			return "Domain is any text"s;

		default:
			return utils::string::va("unhandled dvar type '%i'", type);
		}
	}


	namespace overrides
	{
		std::unordered_map<std::string, dvar_bool> register_bool_overrides;
		std::unordered_map<std::string, dvar_int> register_int_overrides;
		std::unordered_map<std::string, dvar_float> register_float_overrides;

		void register_bool(const std::string& name, const bool value, const unsigned int flags)
		{
			dvar_bool values{};
			values.value = value;
			values.flags = flags;
			register_bool_overrides[name] = std::move(values);
		}

		void register_int(const std::string& name, const int value, const int min, const int max, const unsigned int flags)
		{
			dvar_int values{};
			values.value = value;
			values.max = max;
			values.min = min;
			values.flags = flags;
			register_int_overrides[name] = std::move(values);
		}

		void register_float(const std::string& name, const float value, const float min, const float max, const unsigned int flags)
		{
			dvar_float values;
			values.value = value;
			values.min = min;
			values.max = max;
			values.flags = flags;
			register_float_overrides[name] = std::move(values);
		}
	}

	game::dvar_s* Dvar_RegisterFloat(const char* dvar_name, const char* description, float float_value, float min_value, float max_value, std::uint16_t flags)
	{
		game::DvarValue value{};
		value.value = float_value;

		game::DvarLimits domain{};
		domain.value.max = max_value;
		domain.value.min = min_value;
		console::debug("registered dvar '%s'\n", dvar_name);
		return game::Dvar_RegisterNew(dvar_name, game::DvarType::DVAR_TYPE_FLOAT, flags, description, 0, value, domain);
	}

	game::dvar_s* Dvar_RegisterVec4(const char* dvar_name, const char* description, float x, float y, float z, float w, float min_value, float max_value, std::uint16_t flags)
	{
		game::DvarValue value{};
		value.vector[0] = x;
		value.vector[1] = y;
		value.vector[2] = z;
		value.vector[3] = w;

		game::DvarLimits domain{};
		domain.vector.max = max_value;
		domain.vector.min = min_value;
		console::debug("registered dvar '%s'\n", dvar_name);
		return game::Dvar_RegisterNew(dvar_name, game::DvarType::DVAR_TYPE_FLOAT_4, flags, description, 0, value, domain);
	}

	game::dvar_s* Dvar_RegisterBool(const char* dvar_name, int value_default, const char* description, std::uint16_t flags)
	{
		game::DvarValue value{};
		value.enabled = value_default;

		game::DvarLimits domain{};
		domain.integer.max = 1;
		domain.integer.min = 0;
		console::debug("registered dvar '%s'\n", dvar_name);
		return game::Dvar_RegisterNew(dvar_name, game::DvarType::DVAR_TYPE_BOOL, flags, description, 0, value, domain);
	}

	char* Dvar_ValueToString(game::dvar_s* dvar, game::DvarValue value)
	{
		unsigned int _func = game::game_offset(0x10274F80);
		char* result;
		_asm
		{
			push value;
			mov ecx, dvar;
			call _func;
			add esp, 4;
			mov result, eax;
		}

		return result;
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::once([]
				{
					dvars::Dvar_RegisterBool("g_debugVelocity", 0, "[DEBUG] Print velocity information to console", game::dvar_flags::none);
					dvars::Dvar_RegisterBool("g_debugLocalization", 0, "[DEBUG] Print information to console about unlocalized strings", game::dvar_flags::none);
				}, scheduler::main);
		}
	};
}

REGISTER_COMPONENT(dvars::component)