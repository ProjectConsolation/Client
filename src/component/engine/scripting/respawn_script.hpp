#pragma once
#include <initializer_list>
#include <string>

namespace gametypes::respawn_script
{
	inline bool make_killcam_live(std::string& source)
	{
		// QoS PC archives snapshots in 102EE4E0 and consumes archived player
		// state in 102F3010. Its common_mp script disables the existing replay.
		// Keep that replay/HUD/cleanup implementation; change only its policy.
		if (source.find("consolationKillcamPolicy") != std::string::npos) return false;
		auto patched = source;
		for (auto at = patched.find('\r'); at != std::string::npos; at = patched.find('\r'))
			patched.erase(at, 1);
		const auto init = patched.find("init()\n{");
		const auto update = patched.find("\nupdateKillcamSettings()\n", init);
		const std::string entry = "if(attackerNum < 0)";
		const std::string timer = "self thread waitKillcamTime();";
		if (init != 0 || update == std::string::npos
			|| patched.substr(init, update - init).find("level.killcam = 0;") == std::string::npos)
			return false;
		for (const auto& target : {entry, timer})
		{
			const auto at = patched.find(target);
			if (at == std::string::npos || patched.find(target, at + target.size()) != std::string::npos)
				return false;
		}
		patched.replace(init, update - init,
			"init()\n{\n\tprecacheString(&\"MP_KILLCAM\");\n"
			"\tprecacheString(&\"PLATFORM_PRESS_TO_SKIP\");\n"
			"\tprecacheString(&\"PLATFORM_PRESS_TO_RESPAWN\");\n\tprecacheShader(\"black\");\n"
			"\tlevel.killcam = getDvarInt( \"sv_allowKillcams\" ) != 0;\n"
			"\tlevel thread consolationKillcamPolicy();\n}\n");
		patched.replace(patched.find(entry), entry.size(),
			"if(attackerNum < 0 || !getDvarInt( \"sv_allowKillcams\" ))");
		patched.replace(patched.find(timer), timer.size(),
			"self thread waitKillcamTime();\n\tself thread consolationKillcamDisable();");
		patched += "\n// Consolation: server policy, independent of archived snapshot recording.\n"
			"consolationKillcamPolicy()\n{\n\tfor (;;)\n\t{\n"
			"\t\tlevel.killcam = getDvarInt( \"sv_allowKillcams\" ) != 0;\n\t\twait 0.05;\n\t}\n}\n"
			"consolationKillcamDisable()\n{\n\tself endon( \"disconnect\" );\n"
			"\tself endon( \"end_killcam\" );\n\tself endon( \"spawned\" );\n"
			"\twhile ( getDvarInt( \"sv_allowKillcams\" ) ) wait 0.05;\n"
			"\tself notify( \"end_killcam\" );\n}\n";
		source.swap(patched);
		return true;
	}

	inline bool make_delay_live(std::string& source)
	{
		// QoS PC common_mp _tweakables.gsc caches level.playerTweaks.value.
		// Modify only its named getter; preserve every other tweakable policy.
		const std::string signature = "getTweakableValue( category, name )";
		const auto start = source.find(signature);
		if (start == std::string::npos || source.find(signature, start + signature.size()) != std::string::npos)
			return false;
		const auto brace = source.find('{', start + signature.size());
		if (brace == std::string::npos) return false;
		const auto gap = source.substr(start + signature.size(), brace - start - signature.size());
		if (gap.find_first_not_of(" \t\r\n") != std::string::npos) return false;
		const std::string patch = "\n\t// Consolation: live respawn delay, rather than the initialization cache.\n"
			"\tif ( category == \"player\" && name == \"respawndelay\" )\n"
			"\t{\n\t\tdelay = getDvarInt( \"scr_player_respawndelay\" );\n"
			"\t\tif ( delay < 0 ) delay = 0;\n\t\treturn delay;\n\t}\n"
			"\tif ( category == \"player\" && name == \"forcerespawn\" )\n"
			"\t\treturn getDvarInt( \"scr_player_forcerespawn\" ) != 0;\n";
		if (source.find("// Consolation: live respawn delay") != std::string::npos) return false;
		source.insert(brace + 1, patch);
		return true;
	}

	inline bool make_wait_live(std::string& source)
	{
		// QoS common_mp _globallogic.gsc: both player-delay branches. Keep
		// native round/life and team-wave gates; don't replace its spawn routine.
		if (source.find("consolationWaitRespawnDelay") != std::string::npos) return false;
		const std::string waits[] = {"wait( respawnDelay );", "wait ( respawnDelay );",
			"self waitRespawnButton();", "wait delay;\t// let the player watch themselves die."};
		for (const auto& wait : waits)
		{
			const auto at = source.find(wait);
			if (at == std::string::npos || source.find(wait, at + wait.size()) != std::string::npos) return false;
		}
		for (int i = 0; i < 2; ++i)
			source.replace(source.find(waits[i]), waits[i].size(), "self consolationWaitRespawnDelay();");
		source.replace(source.find(waits[2]), waits[2].size(), "self consolationWaitRespawnButton();");
		source.replace(source.find(waits[3]), waits[3].size(),
			"self consolationWaitDeathTransition( delay ); // let the player watch themselves die.");
		source += "\n// Consolation: console edits also shorten an active player-delay wait.\n"
			"consolationWaitRespawnDelay()\n{\n"
			"\tself endon( \"disconnect\" );\n\tself endon( \"end_respawn\" );\n"
			"\tstarted = getTime();\n"
			"\tfor (;;)\n\t{\n\t\tdelay = getDvarInt( \"scr_player_respawndelay\" );\n"
			"\t\tif ( delay <= 0 || getTime() - started >= delay * 1000 ) return;\n"
			"\t\twait 0.05;\n\t}\n}\n"
			"\nconsolationWaitRespawnButton()\n{\n"
			"\tself endon( \"disconnect\" );\n\tself endon( \"end_respawn\" );\n"
			// QoS maps/mp/_utility::waitRespawnButton uses useButtonPressed,
			// not attack/jump. Keep that input and allow live forced respawn.
			"\twhile ( !self useButtonPressed() && !getDvarInt( \"scr_player_forcerespawn\" ) )\n"
			"\t\twait 0.05;\n}\n"
			"\nconsolationWaitDeathTransition( nativeDelay )\n{\n"
			"\tself endon( \"disconnect\" );\n\tstarted = getTime();\n"
			// Yield at least one server frame: the native comment explicitly
			// requires Callback_PlayerKilled to complete before spawning.
			"\twait 0.05;\n"
			"\twhile ( getTime() - started < nativeDelay * 1000 )\n\t{\n"
			"\t\tif ( !level.killcam && getDvarInt( \"scr_player_respawndelay\" ) <= 0 ) return;\n"
			"\t\twait 0.05;\n\t}\n}\n";
		return true;
	}
}
