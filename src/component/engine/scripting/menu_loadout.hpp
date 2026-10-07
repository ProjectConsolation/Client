#pragma once
#include <string_view>

namespace menu_loadout
{
    constexpr bool is_lobby(std::string_view name)
    {
        return name == "csl_host" || name == "csl_online" || name == "csl_private";
    }

    // QoS's extracted PC frontend menuData defines mpcustomloadout as
    // the editor and mploadout as its LIVE-gated wrapper. No new GFx ABI.
    constexpr std::string_view script(std::string_view lobby)
    {
        if (lobby == "csl_host")
            return "nosplitscreen; xsignin; set xsigninscreen mpcustomloadout; set modifyingloadouts 1; updategamerprofile; cg_updateAvailableMoney; sf_open mpcustomloadout\n";
        return "nosplitscreen; set xsigninscreen mploadout; set modifyingloadouts 1; xsigninlive; sf_open mploadout\n";
    }
}
