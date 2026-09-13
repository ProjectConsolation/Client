#pragma once

#include "game/game.hpp"

#include <string>
#include <unordered_map>

// Limited runtime loader for macro-free IW-style .menu files. This is not a
// complete CoD4/IW4x compiler: preprocessing and expression compilation remain
// unsupported, and complex controls still need runtime verification in QoS.
//
// Menus are looked up by their declared `name` field (matching how the engine
// resolves ASSET_TYPE_MENU by name, not by file path), and every custom menu
// is exposed so a MenuList (e.g. ui_mp/menus_sf.txt) can be augmented with it.
namespace menu_file
{
	// Parses every *.menu file found under the registered filesystem search
	// paths (see filesystem::get_search_paths()) into native menuDef_t trees.
	// Cheap to call repeatedly: only does work the first time, or after reload().
	void ensure_loaded();

	// Forces a full re-parse of every *.menu file on disk. Wired to the
	// "reloadMenus" console command so menus can be iterated on without a
	// full game restart.
	// Refuses while a custom menu is on the native open stack. Closed custom
	// entries are detached before reparsing; stock menu entries are preserved.
	bool reload();

	// Looks up an already-parsed, natively-built menu by its declared name
	// (case-insensitive). Returns nullptr if no disk menu declared that name.
	game::menuDef_t* find(const std::string& name);

	// Every currently loaded custom menu, keyed by lowercase name.
	const std::unordered_map<std::string, game::menuDef_t*>& all();
}
