#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include <component/utils/scheduler.hpp>

#include <utils/string.hpp>

#include "game.hpp"
#include "dvars.hpp"
#include <component/engine/console/console.hpp>
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
	game::dvar_s* bot_maxHealth = nullptr;
	game::dvar_s* m_rawInput = nullptr;
	game::dvar_s* gpad_enabled = nullptr;
	game::dvar_s* gpad_present = nullptr;
	game::dvar_s* gpad_in_use = nullptr;
	game::dvar_s* gpad_debug = nullptr;
	game::dvar_s* gpad_buttonsConfig = nullptr;
	game::dvar_s* gpad_sticksConfig = nullptr;
	game::dvar_s* gpad_rumble = nullptr;
	game::dvar_s* gpad_stick_deadzone_min = nullptr;
	game::dvar_s* gpad_stick_deadzone_max = nullptr;
	game::dvar_s* gpad_button_deadzone = nullptr;
	game::dvar_s* gpad_menu_scroll_delay_first = nullptr;
	game::dvar_s* gpad_menu_scroll_delay_rest = nullptr;
	game::dvar_s* gpad_menu_scroll_delay_min = nullptr;
	game::dvar_s* gpad_menu_scroll_accel_time = nullptr;
	game::dvar_s* input_invertPitch = nullptr;
	game::dvar_s* cg_drawWatermark = nullptr;
	game::dvar_s* cg_drawVersion = nullptr;
	game::dvar_s* cg_drawVersionX = nullptr;
	game::dvar_s* cg_drawVersionY = nullptr;
	game::dvar_s* r_aspectRatioCustomEnable = nullptr;
	game::dvar_s* r_aspectRatioCustom = nullptr;
	game::dvar_s* r_ultrawideCustomMode = nullptr;

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
		std::unordered_map<std::string, dvar_enum> register_enum_overrides;
		std::unordered_map<std::string, dvar_string> register_string_overrides;

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

		void register_enum(const std::string& name, const char* const* value_list, const int default_index, const unsigned int flags)
		{
			dvar_enum values{};
			values.value_list = value_list;
			values.default_index = default_index;
			values.flags = flags;
			register_enum_overrides[name] = std::move(values);
		}

		void register_string(const std::string& name, const std::string& value, const unsigned int flags)
		{
			dvar_string values{};
			values.value = value;
			values.flags = flags;
			register_string_overrides[name] = std::move(values);
		}
	}

	game::dvar_s* Dvar_RegisterFloat(const char* dvar_name, const char* description, double float_value, double min_value, double max_value, std::uint16_t flags)
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

	game::dvar_s* Dvar_RegisterInt(const char* dvar_name, const char* description, int value_default, int min, int max, std::uint16_t flags)
	{
		game::DvarValue value{};
		value.integer = value_default;

		game::DvarLimits domain{};
		domain.integer.max = max;
		domain.integer.min = min;
		console::debug("registered dvar '%s'\n", dvar_name);
		return game::Dvar_RegisterNew(dvar_name, game::DvarType::DVAR_TYPE_INT, flags, description, 0, value, domain);
	}

	game::dvar_s* Dvar_RegisterString(const char* dvar_name, const char* value_default, const char* description, std::uint16_t flags)
	{
		game::DvarValue value{};
		value.string = value_default;

		game::DvarLimits domain{};
		console::debug("registered dvar '%s'\n", dvar_name);
		return game::Dvar_RegisterNew(dvar_name, game::DvarType::DVAR_TYPE_STRING, flags, description, 0, value, domain);
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

	dvar_spec make_bool_spec(const char* name, const char* description, const bool value, const std::uint16_t flags)
	{
		dvar_spec spec{};
		spec.type = game::DVAR_TYPE_BOOL;
		spec.name = name;
		spec.description = description;
		spec.flags = flags;
		spec.value.enabled = value;
		spec.domain.integer.min = 0;
		spec.domain.integer.max = 1;
		return spec;
	}

	dvar_spec make_int_spec(const char* name, const char* description, const int value, const int min, const int max, const std::uint16_t flags)
	{
		dvar_spec spec{};
		spec.type = game::DVAR_TYPE_INT;
		spec.name = name;
		spec.description = description;
		spec.flags = flags;
		spec.value.integer = value;
		spec.domain.integer.min = min;
		spec.domain.integer.max = max;
		return spec;
	}

	dvar_spec make_float_spec(const char* name, const char* description, const float value, const float min, const float max, const std::uint16_t flags)
	{
		dvar_spec spec{};
		spec.type = game::DVAR_TYPE_FLOAT;
		spec.name = name;
		spec.description = description;
		spec.flags = flags;
		spec.value.value = value;
		spec.domain.value.min = min;
		spec.domain.value.max = max;
		return spec;
	}

	dvar_spec make_string_spec(const char* name, const char* description, const char* value, const std::uint16_t flags)
	{
		dvar_spec spec{};
		spec.type = game::DVAR_TYPE_STRING;
		spec.name = name;
		spec.description = description;
		spec.flags = flags;
		spec.value.string = value;
		return spec;
	}

	namespace
	{
		void set_saved_dvar_flags(game::dvar_s* dvar, const std::uint16_t desired_flags)
		{
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | desired_flags);
		}
	}

	game::dvar_s* replace_dvar(const dvar_spec& spec)
	{
		auto* const existing = game::Dvar_FindVar(spec.name);
		if (existing)
		{
			switch (spec.type)
			{
			case game::DVAR_TYPE_BOOL:
				existing->current.enabled = spec.value.enabled;
				existing->latched.enabled = spec.value.enabled;
				existing->reset.enabled = spec.value.enabled;
				break;

			case game::DVAR_TYPE_INT:
				existing->current.integer = spec.value.integer;
				existing->latched.integer = spec.value.integer;
				existing->reset.integer = spec.value.integer;
				break;

			case game::DVAR_TYPE_FLOAT:
				existing->current.value = spec.value.value;
				existing->latched.value = spec.value.value;
				existing->reset.value = spec.value.value;
				break;

			case game::DVAR_TYPE_STRING:
				existing->current.string = spec.value.string;
				existing->latched.string = spec.value.string;
				existing->reset.string = spec.value.string;
				break;

			default:
				break;
			}

			existing->domain = spec.domain;
			existing->modified = true;
			set_saved_dvar_flags(existing, spec.flags);
			return existing;
		}

		switch (spec.type)
		{
		case game::DVAR_TYPE_BOOL:
			return Dvar_RegisterBool(spec.name, spec.value.enabled ? 1 : 0, spec.description, spec.flags);

		case game::DVAR_TYPE_INT:
			return Dvar_RegisterInt(spec.name, spec.description, spec.value.integer, spec.domain.integer.min, spec.domain.integer.max, spec.flags);

		case game::DVAR_TYPE_FLOAT:
			return Dvar_RegisterFloat(spec.name, spec.description, spec.value.value, spec.domain.value.min, spec.domain.value.max, spec.flags);

		case game::DVAR_TYPE_STRING:
			return Dvar_RegisterString(spec.name, spec.value.string ? spec.value.string : "", spec.description, spec.flags);

		default:
			return nullptr;
		}
	}

	game::dvar_s* replace_dvar_at(const std::uintptr_t nop_address, const std::size_t nop_size, game::dvar_s** target, const dvar_spec& spec)
	{
		utils::hook::nop(nop_address, nop_size);
		auto* const dvar = replace_dvar(spec);
		if (target)
		{
			*target = dvar;
		}

		return dvar;
	}



	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			dvars::overrides::register_bool("monkeytoy", false, game::dvar_flags::none);

			scheduler::once([]
				{
					if (auto* const existing_borderless = game::Dvar_FindVar("r_borderless"))
					{
						r_borderless = existing_borderless;
					}
					else
					{
						r_borderless = dvars::Dvar_RegisterBool("r_borderless", 0, "Do not use a border in windowed mode", game::dvar_flags::saved);
					}
					dvars::Dvar_RegisterBool("g_debugVelocity", 0, "[DEBUG] Print velocity information to console", game::dvar_flags::none);
					dvars::Dvar_RegisterBool("g_debugLocalization", 0, "[DEBUG] Print information to console about unlocalized strings", game::dvar_flags::none);
					bot_maxHealth = dvars::Dvar_RegisterInt("bot_maxHealth", "Maximum health for bots on spawn", 100, 1, 1000, game::dvar_flags::none);
					m_rawInput = dvars::Dvar_RegisterBool("m_rawInput", 1, "Use raw mouse input.", game::dvar_flags::saved);
					gpad_enabled = dvars::Dvar_RegisterBool("gpad_enabled", 1, "Enable XInput gamepad input.", game::dvar_flags::saved);
					gpad_present = dvars::Dvar_RegisterBool("gpad_present", 0, "True when a supported gamepad is connected.", game::dvar_flags::none);
					gpad_in_use = dvars::Dvar_RegisterBool("gpad_in_use", 0, "True when a gamepad is currently being used.", game::dvar_flags::none);
					gpad_debug = dvars::Dvar_RegisterBool("gpad_debug", 0, "Print gamepad debug information.", game::dvar_flags::none);
					gpad_buttonsConfig = dvars::Dvar_RegisterString("gpad_buttonsConfig", "buttons_default_alt", "Active gamepad button layout preset.", game::dvar_flags::saved);
					gpad_sticksConfig = dvars::Dvar_RegisterString("gpad_sticksConfig", "thumbstick_default", "Active gamepad stick layout preset.", game::dvar_flags::saved);
					gpad_rumble = dvars::Dvar_RegisterBool("gpad_rumble", 1, "Enable controller rumble.", game::dvar_flags::saved);
					gpad_stick_deadzone_min = dvars::Dvar_RegisterFloat("gpad_stick_deadzone_min", "Minimum stick deadzone for menu navigation.", 0.2f, 0.0f, 1.0f, game::dvar_flags::saved);
					gpad_stick_deadzone_max = dvars::Dvar_RegisterFloat("gpad_stick_deadzone_max", "Maximum stick deadzone for native gamepad movement.", 0.01f, 0.0f, 1.0f, game::dvar_flags::saved);
					gpad_button_deadzone = dvars::Dvar_RegisterFloat("gpad_button_deadzone", "Trigger deadzone for native gamepad buttons.", 0.13f, 0.0f, 1.0f, game::dvar_flags::saved);
					gpad_menu_scroll_delay_first = dvars::Dvar_RegisterInt("gpad_menu_scroll_delay_first", "Initial menu repeat delay for gamepad input in milliseconds.", 420, 0, 1000, game::dvar_flags::saved);
					gpad_menu_scroll_delay_rest = dvars::Dvar_RegisterInt("gpad_menu_scroll_delay_rest", "Subsequent menu repeat delay for gamepad input in milliseconds.", 210, 0, 1000, game::dvar_flags::saved);
					gpad_menu_scroll_delay_min = dvars::Dvar_RegisterInt("gpad_menu_scroll_delay_min", "Minimum accelerated menu repeat delay for gamepad input in milliseconds.", 50, 0, 1000, game::dvar_flags::saved);
					gpad_menu_scroll_accel_time = dvars::Dvar_RegisterInt("gpad_menu_scroll_accel_time", "Time in milliseconds for accelerated gamepad menu repeat to reach full speed.", 1500, 0, 5000, game::dvar_flags::saved);
					input_invertPitch = dvars::Dvar_RegisterBool("input_invertPitch", 0, "Invert native gamepad pitch.", game::dvar_flags::saved);
					cg_drawWatermark = dvars::Dvar_RegisterBool("cg_drawWatermark", 1, "Draw the Consolation watermark in the bottom-right corner.", game::dvar_flags::saved);
			cg_drawVersion = dvars::Dvar_RegisterBool("cg_drawVersion", 1, "Draw the game version.", game::dvar_flags::saved);
			cg_drawVersionX = dvars::Dvar_RegisterFloat("cg_drawVersionX", "X offset for the version string.", 50.0f, -1024.0f, 1024.0f, game::dvar_flags::saved);
			cg_drawVersionY = dvars::Dvar_RegisterFloat("cg_drawVersionY", "Y offset for the version string.", 18.0f, -1024.0f, 1024.0f, game::dvar_flags::saved);
					replace_dvar(make_int_spec("g_speed", "Player movement speed", 210, 0, 1000, game::dvar_flags::saved));
					replace_dvar(make_float_spec("ui_smallFont", "Small UI font scale", 0.0f, 0.0f, 1.0f, game::dvar_flags::saved));
					replace_dvar(make_float_spec("ui_bigFont", "Large UI font scale", 0.0f, 0.0f, 1.0f, game::dvar_flags::saved));
					replace_dvar(make_float_spec("ui_extraBigFont", "Extra-large UI font scale", 0.0f, 0.0f, 1.0f, game::dvar_flags::saved));
					replace_dvar(make_float_spec("cg_overheadNamesSize", "Overhead name font scale", 0.5f, 0.0f, 1.0f, game::dvar_flags::saved));
					replace_dvar(make_float_spec("input_viewSensitivity", "Mouse sensitivity", 1.0f, 0.01f, 30.0f, game::dvar_flags::saved));
				}, scheduler::main);

			scheduler::on_shutdown([]
				{
					bot_maxHealth = nullptr;
					m_rawInput = nullptr;
					gpad_enabled = nullptr;
					gpad_present = nullptr;
					gpad_in_use = nullptr;
					gpad_debug = nullptr;
					gpad_buttonsConfig = nullptr;
					gpad_sticksConfig = nullptr;
					gpad_rumble = nullptr;
					gpad_stick_deadzone_min = nullptr;
					gpad_stick_deadzone_max = nullptr;
					gpad_button_deadzone = nullptr;
					gpad_menu_scroll_delay_first = nullptr;
					gpad_menu_scroll_delay_rest = nullptr;
					gpad_menu_scroll_delay_min = nullptr;
					gpad_menu_scroll_accel_time = nullptr;
					input_invertPitch = nullptr;
					cg_drawWatermark = nullptr;
					cg_drawVersion = nullptr;
					cg_drawVersionX = nullptr;
					cg_drawVersionY = nullptr;
					r_aspectRatioCustomEnable = nullptr;
					r_aspectRatioCustom = nullptr;
					r_ultrawideCustomMode = nullptr;
				});
		}
	};
}

REGISTER_COMPONENT(dvars::component)
