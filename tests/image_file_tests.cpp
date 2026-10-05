#include "../src/component/engine/renderer/image_file.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>

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

    void put32(std::vector<unsigned char>& data, const std::size_t offset, const unsigned value)
    {
        for (unsigned b = 0; b < 4; ++b) data[offset + b] = (value >> (8 * b)) & 255;
    }

    std::vector<unsigned char> dds_fixture(const bool cube)
    {
        std::vector<unsigned char> data(128 + (cube ? 6 : 1) * 24, 0);
        put32(data, 0, 0x20534444); put32(data, 4, 124);
        put32(data, 12, 4); put32(data, 16, 4); put32(data, 28, 3);
        put32(data, 76, 32); put32(data, 80, 4); put32(data, 84, 0x31545844);
        put32(data, 112, cube ? 0xFE00 : 0);
        for (unsigned face = 0; face < (cube ? 6u : 1u); ++face)
            for (unsigned level = 0; level < 3; ++level)
                std::fill_n(data.begin() + 128 + face * 24 + level * 8, 8,
                    static_cast<unsigned char>(face * 3 + level));
        return data;
    }

    bool dds_rejected(const std::vector<unsigned char>& data)
    {
        try { image_overrides::decode_dds(data); }
        catch (const std::runtime_error&) { return true; }
        return false;
    }
}

int main(int argc, char** argv)
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
    assert(image_filename("*lightmap0_primary") == "%2Alightmap0_primary");
    assert(image_filename("a%2Ab") == "a%252Ab");
    for (const auto name : {"", "../brick", "a/../b", "C:\\brick", "/brick", "a//b", ",brick", "a/", "a.", "a "})
        assert(!safe_image_name(name));
    for (const bool cube : {false, true})
    {
        const auto input = dds_fixture(cube);
        const auto output = decode_dds(input);
        load_definition header{};
        std::memcpy(&header, output.data(), sizeof(header));
        assert(header.level_count == 3 && header.format == 0x31545844);
        assert(((header.flags & 4) != 0) == cube);
        const unsigned faces = cube ? 6 : 1;
        for (unsigned index = 0; index < 3 * faces; ++index)
            assert(output[16 + index * 8] == (index % faces) * 3 + 2 - index / faces);
    }
    auto dds = dds_fixture(false);
    put32(dds, 28, 2); assert(dds_rejected(dds));
    dds = dds_fixture(true); put32(dds, 112, 0x600); assert(dds_rejected(dds));
    dds = dds_fixture(false); dds.pop_back(); assert(dds_rejected(dds));
    dds = dds_fixture(false); put32(dds, 84, 0x32495441); assert(dds_rejected(dds)); // ATI2/BC5
    dds = dds_fixture(false); put32(dds, 24, 8); assert(dds_rejected(dds));
    // Legacy RGBA mask conversion, straight alpha unchanged.
    dds.assign(132, 0);
    put32(dds, 0, 0x20534444); put32(dds, 4, 124);
    put32(dds, 12, 1); put32(dds, 16, 1); put32(dds, 76, 32);
    put32(dds, 80, 0x41); put32(dds, 88, 32);
    put32(dds, 92, 0xFF); put32(dds, 96, 0xFF00);
    put32(dds, 100, 0xFF0000); put32(dds, 104, 0xFF000000);
    dds[128] = 255; dds[131] = 128;
    const auto bgra = decode_dds(dds);
    assert(bgra[16] == 0 && bgra[18] == 255 && bgra[19] == 128);
    // DX10 RGBA single 2D image and array rejection.
    dds.resize(152, 0);
    put32(dds, 80, 4); put32(dds, 84, 0x30315844);
    put32(dds, 128, 28); put32(dds, 132, 3); put32(dds, 140, 1);
    dds[148] = 255; dds[151] = 128;
    assert(decode_dds(dds) == bgra);
    put32(dds, 140, 2); assert(dds_rejected(dds));
    if (argc == 2)
    {
        unsigned count = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1]))
        {
            const auto extension = entry.path().extension();
            if (extension != ".dds" && extension != ".iwi") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            assert(file);
            const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
            const auto result = extension == ".dds" ? decode_dds(bytes) : decode_iwi(bytes);
            assert(result.size() > 16);
            ++count;
        }
        assert(count != 0);
        std::cout << "Validated " << count << " dumped DDS/IWI files\n";
    }
    std::cout << "IWI/DDS override parser tests passed\n";
}
