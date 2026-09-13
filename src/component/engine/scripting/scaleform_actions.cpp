#include <std_include.hpp>

#include "loader/component_loader.hpp"
#include "component/engine/console/command.hpp"
#include "game/game.hpp"

#include <utils/hook.hpp>

namespace scaleform_actions
{
	namespace
	{
		utils::hook::detour action_dispatch_hook;
		using action_dispatch_t = void(__stdcall*)(int, const char*, const char*);

		bool is_server_browser_action(const char* action)
		{
			return action != nullptr
				&& (_stricmp(action, "open_server_browser") == 0
					|| _stricmp(action, "openmenu_serverbrowser") == 0);
		}

		bool handle_action(const char* action)
		{
			if (!is_server_browser_action(action))
			{
				return false;
			}

			// Native openmenu only searches the active UI context. The browser must
			// already be parsed and registered; this action does not load its file.
			command::execute("openmenu serverbrowser");
			game::Com_Printf(13, "[scaleform - actions] requested: openmenu serverbrowser\n");
			return true;
		}

		// QoS 0x10002280 reads the action from [ebp+0Ch] and returns with
		// retn 0Ch. EDI is populated by its prologue, not by the caller.
		void __stdcall action_dispatch_stub(int context, const char* action, const char* argument)
		{
			if (!handle_action(action))
			{
				reinterpret_cast<action_dispatch_t>(action_dispatch_hook.get_original())(context, action, argument);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			action_dispatch_hook.create(game::game_offset(0x10002280), action_dispatch_stub);
			game::Com_Printf(13, "[scaleform - actions] installed: server browser action dispatch\n");
		}
	};
}

REGISTER_COMPONENT(scaleform_actions::component);
