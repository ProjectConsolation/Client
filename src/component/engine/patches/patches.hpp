#pragma once

namespace game
{
	struct usercmd_t;
}

namespace patches
{
	void enforce_ads_sprint_interrupt(game::usercmd_t* cmd);
}
