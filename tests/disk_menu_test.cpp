#include "../src/component/engine/scripting/disk_menu_test.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 3;
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto parsed = disk_menu_test::parser(source).parse();
    if (parsed.name != "disk_menu_test" || parsed.items.size() != 2
        || parsed.items[0].text != "DISK MENU TEST" || parsed.items[0].button
        || !parsed.items[1].button || parsed.items[1].text != "BACK") return 4;

    auto rejects = [](const std::string& text)
    {
        try { disk_menu_test::parser(text).parse(); }
        catch (const std::runtime_error&) { return true; }
        return false;
    };
    auto replace = [&](const std::string& from, const std::string& to)
    {
        auto text = source;
        const auto pos = text.find(from);
        if (pos == std::string::npos) throw std::runtime_error("test fixture changed");
        text.replace(pos, from.size(), to);
        return text;
    };
    const std::vector<std::string> invalid{
        "", "#include \"ui/menudef.h\"\n" + source,
        source + " menuDef {}", source.substr(0, source.rfind('}')),
        replace("0.5", "NaN"), replace("0.5", "0.5junk"), replace("0.5", "0"),
        replace("80 120 480 48", "80 120 -1 48"),
        replace("80 120 480 48", "80 120 700 48"),
        replace("decoration", "visible 1"),
        replace("ITEM_TYPE_BUTTON", "ITEM_TYPE_LISTBOX"),
        replace("close disk_menu_test;", "exec quit;"),
        replace("\"back\"", "\"title\""),
        replace("DISK MENU TEST\"", "DISK MENU TEST"),
        source + std::string(1, '\0')
    };
    for (std::size_t i = 0; i < invalid.size(); ++i)
    {
        if (!rejects(invalid[i]))
        {
            std::cerr << "Accepted invalid case " << i << '\n';
            return 5;
        }
    }
    std::cout << "Disk menu parser: valid fixture and " << invalid.size() << " rejection cases passed\n";
}
