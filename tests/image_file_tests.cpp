#include "../src/component/engine/renderer/image_file.hpp"
#include <cassert>
#include <iostream>

namespace
{
    std::vector<unsigned char> fixture(const unsigned char format, const bool cube, const bool mips)
    {
        const unsigned block = format == 11 ? 8 : 16;
        const unsigned faces = cube ? 6 : 1;
        const unsigned levels = mips ? 3 : 1; // 4x4, 2x2, 1x1
        std::vector<unsigned char> data(28 + block * faces * levels, 0);
        data[0] = 'I'; data[1] = 'W'; data[2] = 'i'; data[3] = 6;
        data[4] = format; data[5] = (cube ? 4 : 0) | (mips ? 0 : 2);
        data[6] = data[8] = 4; data[10] = 1;
        for (unsigned p = 0; p < 4; ++p)
        {
            const unsigned size = 28 + block * faces * (levels - std::min(p, levels - 1));
            for (unsigned b = 0; b < 4; ++b) data[12 + p * 4 + b] = (size >> (8 * b)) & 255;
        }
        for (std::size_t i = 28; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i);
        return data;
    }

    bool rejected(const std::vector<unsigned char>& data)
    {
        try { image_overrides::decode_iwi(data); }
        catch (const std::runtime_error&) { return true; }
        return false;
    }
}

int main()
{
    using namespace image_overrides;
    for (unsigned char format = 11; format <= 13; ++format)
        for (const bool cube : {false, true})
            for (const bool mips : {false, true})
            {
                const auto input = fixture(format, cube, mips);
                const auto output = decode_iwi(input);
                load_definition header{};
                std::memcpy(&header, output.data(), sizeof(header));
                assert(header.format == (format == 11 ? 0x31545844u : format == 12 ? 0x33545844u : 0x35545844u));
                assert(header.level_count == (mips ? 3 : 1));
                assert(header.width == 4 && header.height == 4 && header.depth == 1);
                assert(header.resource_size == input.size() - 28);
                assert(std::equal(input.begin() + 28, input.end(), output.begin() + 16));
            }
    auto bad = fixture(11, false, true);
    auto tiny = fixture(11, false, false);
    tiny[6] = tiny[8] = 1;
    assert(!rejected(tiny)); // Sub-block textures still occupy one DXT block.
    bad[3] = 8; assert(rejected(bad));
    bad = fixture(11, false, true); bad[5] |= 8; assert(rejected(bad));
    bad = fixture(11, false, true); bad[4] = 14; assert(rejected(bad));
    bad = fixture(11, false, true); bad[16] ^= 1; assert(rejected(bad));
    bad = fixture(11, false, true); bad.pop_back(); assert(rejected(bad));
    bad = fixture(11, false, true); bad[6] = 3; assert(rejected(bad));
    assert(rejected({}));
    assert(safe_image_name("brick_wall_col"));
    assert(safe_image_name("custom/brick_wall_col"));
    for (const auto name : {"", "../brick", "a/../b", "C:\\brick", "/brick", "a//b", ",brick", "a/", "a.", "a "})
        assert(!safe_image_name(name));
    std::cout << "IWI override parser tests passed\n";
}
