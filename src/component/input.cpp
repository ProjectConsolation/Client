#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "game_console.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include "console.hpp"

namespace input
{
	namespace
	{
		utils::hook::detour cl_char_event_hook;
		utils::hook::detour cl_key_event_hook;

		void cl_key_event_stub(const int key, const int down)
		{
			if (!game_console::console_key_event(0, key, down))
			{
				return;
			}

			cl_key_event_hook.invoke<void>(key, down);
		}
		

		void cl_char_event_stub(int unk)
		{
			int _key;
			__asm
			{
				mov _key, ecx;
			}
			

			if (!game_console::console_char_event(0, _key))
			{
				return;
			}

			__asm
			{
				mov ecx, _key;
			}

			cl_char_event_hook.invoke<void>(unk);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			cl_char_event_hook.create(game::game_offset(0x1031A1D0), cl_char_event_stub);
			cl_key_event_hook.create(game::game_offset(0x1031A680), cl_key_event_stub);
		}
	};
}

//REGISTER_COMPONENT(input::component)
//commented cus else i cant do anything, shoot, move, etc
