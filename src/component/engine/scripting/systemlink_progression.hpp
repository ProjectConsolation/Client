#pragma once
#include <string>

namespace gametypes::systemlink_progression
{
	inline bool enable_money(std::string& source)
	{
		// QoS PC common_mp _persistence_util.gsc statGet/statAdd and
		// _globallogic.gsc awardMoney. Preserve native score multipliers,
		// stat indices, setStat's reliable M command, and all ranked-only stats.
		if (source.find("// Consolation: local System Link money") != std::string::npos) return false;
		auto patched = source;
		for (auto at = patched.find('\r'); at != std::string::npos; at = patched.find('\r'))
			patched.erase(at, 1);
		const char* signatures[] = {"statGet( dataBlock, dataName )", "statAdd( dataBlock, dataName, value )"};
		const char* guards[] = {"if ( !level.onlineGame )", "if ( !level.rankedMatch )"};
		for (int i = 0; i < 2; ++i)
		{
			const auto start = patched.find(signatures[i]);
			if (start == std::string::npos || patched.find(signatures[i], start + std::string(signatures[i]).size()) != std::string::npos)
				return false;
			const auto brace = patched.find('{', start);
			const auto end = patched.find('}', brace);
			const auto guard = patched.find(guards[i], brace);
			if (brace == std::string::npos || end == std::string::npos || guard == std::string::npos || guard >= end)
				return false;
			const std::string replacement = std::string("// Consolation: local System Link money, not ranked progression.\n\t")
				+ (i == 0 ? "if ( !level.onlineGame" : "if ( !level.rankedMatch")
				+ " && !(getDvarInt( \"systemlink\" ) && dataBlock == \"stats\" && dataName == \"money\") )";
			patched.replace(guard, std::string(guards[i]).size(), replacement);
		}
		source.swap(patched);
		return true;
	}
}
