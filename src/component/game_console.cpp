#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game_console.hpp"

#include <utils/hook.hpp>

namespace game_console
{
	namespace
	{
		utils::hook::detour cl_key_event_hook;

		void field_clear(game::field_t* field)
		{
			if (!field)
			{
				return;
			}

			std::memset(field->buffer, 0, sizeof(field->buffer));
			field->cursor = 0;
			field->scroll = 0;
			field->drawWidth = 256;
		}

		bool is_console_toggle_key(const int key)
		{
			return key == game::K_GRAVE || key == game::K_TILDE || key == '|' || key == '\\';
		}

		void prepare_console_field()
		{
			field_clear(game::g_consoleField);
			game::Con_CancelAutoComplete();
			game::g_consoleField->widthInPixels = *game::g_console_field_width;
			game::g_consoleField->charHeight = 16.0f;
			game::g_consoleField->fixedSize = 1;
		}

		void close_console()
		{
			*game::keyCatchers &= ~1;
			*game::con_outputVisible = 0;
		}

		void open_console()
		{
			prepare_console_field();
			*game::keyCatchers |= 1;
			*game::con_outputVisible = 1;
		}

		void __cdecl cl_key_event_stub(const int key, const int down, const unsigned int time)
		{
			if (down && is_console_toggle_key(key))
			{
				toggle();
				return;
			}

			cl_key_event_hook.invoke<void>(key, down, time);
		}
	}

	bool is_active()
	{
		return (*game::keyCatchers & 1) != 0;
	}

	void toggle()
	{
		if (is_active())
		{
			close_console();
		}
		else
		{
			open_console();
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			cl_key_event_hook.create(reinterpret_cast<std::uintptr_t>(game::CL_KeyEvent.get()), cl_key_event_stub);
		}

		void pre_destroy() override
		{
			cl_key_event_hook.clear();
		}
	};
}

REGISTER_COMPONENT(game_console::component)
