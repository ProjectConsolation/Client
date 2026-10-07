#pragma once
#include <string_view>

namespace fastfiles::dlc
{
    constexpr bool requires_donor(std::string_view name)
    {
        if (name.ends_with("_load")) name.remove_suffix(5);
        return name == "mp_bags" || name == "mp_canals"
            || name == "mp_cistern" || name == "mp_dam";
    }

    // KisakCOD DB_ZONE_DEV's ownership bit, checked against QoS PC
    // DB_LoadXAssets 103E1CF0 and DB_TryLoadXFile 103E17B0. Preserve
    // QoS's 0x10 image-loading bit, but never share the HUD's 0x01 bit.
    constexpr int donor_free_flags = 0x40;
    constexpr int donor_alloc_flags = donor_free_flags | 0x10;
}
