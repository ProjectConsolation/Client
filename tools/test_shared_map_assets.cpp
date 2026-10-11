#include "../src/component/engine/zones/dlc_zone_policy.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <initializer_list>

int main()
{
    using namespace fastfiles::dlc;
    for (const auto map : {"mp_bags", "mp_canals", "mp_cistern", "mp_dam", "mp_shipment"})
    {
        assert(requires_shared_assets(map));
        assert(requires_shared_assets(std::string(map) + "_load"));
    }
    assert(!requires_shared_assets("mp_ecohotel"));
    assert(!requires_shared_assets("mp_barge_load"));
    assert(!requires_shared_assets("mp_unknown"));
    assert(!requires_donor("mp_shipment"));
    assert(!allows_image_archive("csl_dlc01.iwd", "mp_shipment"));
    assert(!allows_image_archive("csl_canals.iwd", "mp_shipment"));
    assert(allows_image_archive("csl_dlc01.iwd", "mp_canals"));
    assert(donor_free_flags == 0x40 && donor_alloc_flags == 0x50);
    std::puts("Converted-map shared asset policy tests passed");
}
