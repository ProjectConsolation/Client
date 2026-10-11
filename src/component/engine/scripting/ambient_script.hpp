#pragma once
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <string>

namespace gametypes::ambient_script
{
	inline bool stock_map(const std::string& map)
	{
		for (const auto* name : {"mp_barge", "mp_constructionsite", "mp_docks", "mp_ecohotel",
			"mp_embassy", "mp_facility", "mp_italia", "mp_miamiconcourse", "mp_rooftop",
			"mp_sauna", "mp_sciencecenter", "mp_siena"})
			if (map == name) return true;
		return false;
	}

	inline bool restore_loops(std::string& source, std::string placements, const std::string& map,
		const std::function<bool(const std::string&)>& available)
	{
		// Retail v470 map FX scripts put CreateFX startup inside /# ... #/.
		// KisakCOD cg_client_side_effects_mp.cpp instead reads these placements
		// on the client. QoS PC has no corresponding reader/string references.
		// Preserve the authored positions and native playLoopSound replication;
		// do not activate unfinished ambientpackage scripts or replay visual FX.
		if (!stock_map(map) || source.find("consolationAmbientLoops") != std::string::npos) return false;
		auto patched = source;
		patched.erase(std::remove(patched.begin(), patched.end(), '\r'), patched.end());
		placements.erase(std::remove(placements.begin(), placements.end(), '\r'), placements.end());
		const std::string entry = "main()\n{";
		const auto main = patched.find(entry);
		auto folded = patched;
		std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		const auto call = folded.find("maps\\createfx\\" + map + "_fx::main();");
		const auto developer = patched.find("/#");
		const auto end_developer = patched.find("#/", developer);
		if (main == std::string::npos || patched.find(entry, main + entry.size()) != std::string::npos
			|| call == std::string::npos || developer == std::string::npos || call < developer
			|| end_developer == std::string::npos || call > end_developer) return false;

		// Accept only the stock generated four-line sound placement grammar.
		// Numeric-only vectors and identifier-only aliases cannot inject GSC.
		static const std::regex record(R"re(ent\s*=\s*maps\\mp\\_createfx::createLoopSound\(\);\s*ent\.v\[\s*"origin"\s*\]\s*=\s*(\(\s*[-+0-9.eE]+\s*,\s*[-+0-9.eE]+\s*,\s*[-+0-9.eE]+\s*\))\s*;\s*ent\.v\[\s*"angles"\s*\]\s*=\s*\(\s*[-+0-9.eE]+\s*,\s*[-+0-9.eE]+\s*,\s*[-+0-9.eE]+\s*\)\s*;\s*ent\.v\[\s*"soundalias"\s*\]\s*=\s*"([A-Za-z0-9_]+)"\s*;)re");
		std::string body;
		unsigned count = 0;
		for (std::sregex_iterator it(placements.begin(), placements.end(), record), end; it != end; ++it)
		{
			const auto alias = (*it)[2].str();
			if (!available(alias)) continue;
			if (++count > 128) return false;
			body += "\tent = spawn(\"script_origin\", " + (*it)[1].str() + ");\n"
				"\tent playLoopSound(\"" + alias + "\");\n";
		}
		if (!count) return false;
		patched.insert(main + entry.size(), "\n\tconsolationAmbientLoops();");
		patched += "\n// Consolation: retail PC sound-only CreateFX fallback.\n"
			"consolationAmbientLoops()\n{\n"
			"\tif (isdefined(level.consolationAmbientLoops)) return;\n"
			"\tlevel.consolationAmbientLoops = true;\n"
			"\t// The developer-only original runs in this configuration.\n"
			"\tif (getdvarint(\"developer_script\") && getdvar(\"clientSideEffects\") != \"1\") return;\n"
			+ body + "}\n";
		source.swap(patched);
		return true;
	}
}
