#include "../src/component/engine/zones/portal_links.hpp"
#include <cassert>
#include <string>
int main()
{
    std::string data("CSLP", 4);
    auto word = [&](std::uint32_t value) { data.append(reinterpret_cast<const char*>(&value), 4); };
    for (auto value : {1u, 3u, 2u, 0u, 0u, 2u, 2u, 0u, 0u}) word(value);
    fastfiles::portal_links::manifest result;
    assert(fastfiles::portal_links::decode(data, result));
    assert(result.cells == 3 && result.links.size() == 2 && result.links[0].destination == 2);
    for (std::size_t i = 0; i < data.size(); ++i)
        assert(!fastfiles::portal_links::decode(std::span(data.data(), i), result));
    auto invalid = data;
    invalid[24] = 3; // Destination outside root cells.
    assert(!fastfiles::portal_links::decode(invalid, result));
    invalid = data; invalid[4] = 2;
    assert(!fastfiles::portal_links::decode(invalid, result));
    invalid = data; invalid[28] = 0; // Duplicate source portal.
    assert(!fastfiles::portal_links::decode(invalid, result));
    assert(result.links.size() == 2); // Rejection does not partially publish.
}
