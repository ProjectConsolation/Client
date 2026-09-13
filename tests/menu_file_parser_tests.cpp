#define CONSOLATION_MENU_PARSER_TEST
namespace console
{
    template <typename... Args> void warn(const char*, Args...) {}
}
#include "../src/component/engine/scripting/menu_file.cpp"
#include <iostream>
#include <iterator>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::ifstream file(argv[1]);
    if (!file) return 3;
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto menus = menu_file::parse_menu_file(source);
    if (menus.size() != 1 || menus[0].items.size() != 2
        || menus[0].on_esc.find("close disk_menu_test;") == std::string::npos
        || menus[0].items[1].action.find("close disk_menu_test;") == std::string::npos) return 4;
    const auto quoted = menu_file::parse_menu_file(
        "menuDef { name test onEsc { exec \"echo }\"; /* } */ // }\n close test; } }");
    if (quoted.size() != 1 || quoted[0].on_esc.find("close test;") == std::string::npos) return 5;
    const auto visibility = menu_file::parse_menu_file(
        "menuDef { name test visible 1 itemDef { name hidden visible 0 } itemDef { name shown visible 1 } }");
    if (!visibility[0].visible || visibility[0].items[0].visible || !visibility[0].items[1].visible) return 7;
    if (menus[0].visible || !menus[0].items[0].visible) return 8;
    const std::vector<std::string> invalid{
        "#include \"missing.inc\"\n" + source,
        "menuDef { name test onEsc { close test; }",
        "menuDef { name test /* unfinished",
        "menuDef { name test fullscreen 1e30 }",
        "menuDef { name test fullscreen 0.5 }",
        "menuDef { name test unknownField 1 }",
        "menuDef { name test visible 2 }",
        "menuDef { name test visible { 1 } }",
        "menuDef { name test itemDef { name test visible -1 } }",
        "menuDef { name test itemDef { name test visible 0.5 } }",
        "menuDef { name test itemDef { name test disabled { 1 } } }"
    };
    for (const auto& text : invalid)
    {
        bool rejected = false;
        try { menu_file::parse_menu_file(text); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) return 6;
    }
    std::cout << "Menu parser: diagnostic file, quoted/commented braces, visibility, and "
        << invalid.size() << " rejection cases passed\n";
}
