#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include "console.hpp"

namespace input
{
	namespace
	{
		struct con
		{
			bool toggled;
		};

		con game_console{};

		utils::hook::detour cl_char_event_hook;
		utils::hook::detour cl_key_event_hook;

		void Field_Clear(game::field_t* edit)
		{
			memset(edit->buffer, 0, sizeof(edit->buffer));
			edit->cursor = 0;
			edit->scroll = 0;
			edit->drawWidth = 256;
		}

		void Con_ToggleConsole()
		{
			Field_Clear(game::g_consoleField);
			game::Con_CancelAutoComplete();
			game::g_consoleField->widthInPixels = *game::g_console_field_width;
			game::g_consoleField->charHeight = 16.0;
			game::g_consoleField->fixedSize = 1;
			*game::con_outputVisible = 0;
			*game::keyCatchers ^= 1;
			game_console.toggled = true;
		}

		bool Con_IsActive()
		{
			return (*game::keyCatchers & 1) != 0;
		}

		void Con_ToggleConsoleOutput()
		{
			*game::con_outputVisible = ~*game::con_outputVisible;
		}

		void cl_key_event_stub(const int key, const int down, const unsigned int time)
		{
			if (down)
			{
				auto con_restricted = game::Dvar_FindVar("monkeytoy");
				if (con_restricted && !con_restricted->current.enabled)
				{
					if (key == game::K_GRAVE || key == game::K_TILDE)
					{
						if (game::playerKeys[0].keys[game::keyNum_t::K_SHIFT].down)
						{
							if (!Con_IsActive())
								Con_ToggleConsole();
							Con_ToggleConsoleOutput();
							return;
						}

						Con_ToggleConsole();
						return;
					}
				}
			}

			if (Con_IsActive() && game_console.toggled)
			{
				Field_Clear(game::g_consoleField);
			}

			cl_key_event_hook.invoke<void>(key, down, time);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			//scheduler::loop(cg_draw_version, scheduler::pipeline::renderer);
			cl_key_event_hook.create(game::game_offset(0x1031A680), cl_key_event_stub);
		}
	};
}

//REGISTER_COMPONENT(input::component)