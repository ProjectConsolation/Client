#include <Windows.h>
#undef min
#undef max
#include "../src/component/engine/renderer/image_png.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>

int main(int argc, char** argv)
{
    // Generated 1x1 RGBA PNG: red, straight alpha 128; valid PNG CRCs.
    const unsigned char png[]{137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,
        0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,
        120,156,99,248,207,192,208,0,0,4,129,1,128,44,85,206,176,
        0,0,0,0,73,69,78,68,174,66,96,130};
    const auto data = image_overrides::decode_png(png);
    image_overrides::load_definition header{};
    std::memcpy(&header, data.data(), sizeof(header));
    assert(header.width == 1 && header.height == 1 && header.format == 21);
    assert(header.flags == 3 && header.resource_size == 4);
    assert(data[16] == 0 && data[17] == 0 && data[18] == 255 && data[19] == 128);
    bool rejected = false;
    try { image_overrides::decode_png(std::span(png).first(15)); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    // Existing STA must remain intact; decode_png may not uninitialize it.
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    assert(SUCCEEDED(initialized));
    assert(image_overrides::decode_png(png) == data);
    CoUninitialize();
    if (argc == 2)
    {
        unsigned count = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1]))
        {
            if (entry.path().extension() != ".png") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            assert(file);
            const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
            const auto decoded = image_overrides::decode_png(bytes);
            assert(decoded.size() > 16);
            ++count;
        }
        assert(count != 0);
        std::cout << "Validated " << count << " dumped PNG files\n";
    }
    std::cout << "PNG override decoder tests passed\n";
}
