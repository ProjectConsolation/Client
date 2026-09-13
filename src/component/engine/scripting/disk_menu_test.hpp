#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Deliberately limited diagnostic grammar, not a CoD4/IW4x menu importer.
// Keep parsing independent of engine allocations so invalid input never reaches UI.
namespace disk_menu_test
{
    struct item
    {
        std::string name;
        std::string text;
        std::array<float, 4> rect{};
        float scale = 0.35f;
        bool button = false;
    };

    struct definition
    {
        std::string name;
        std::vector<item> items;
    };

    class parser
    {
    public:
        explicit parser(const std::string& source) : source_(source) {}

        definition parse()
        {
            if (source_.find('\0') != std::string::npos) fail("embedded NUL");
            expect("menuDef");
            expect("{");
            expect("name");
            definition result;
            result.name = token();
            if (result.name != "disk_menu_test") fail("expected name disk_menu_test");
            expect("onEsc");
            close_action();
            for (auto field = token(); field != "}"; field = token())
            {
                if (field != "itemDef") fail("expected itemDef or closing brace");
                if (result.items.size() == 16) fail("too many items");
                expect("{");
                expect("name");
                item entry;
                entry.name = token();
                if (entry.name.empty()) fail("empty item name");
                for (const auto& previous : result.items)
                    if (previous.name == entry.name) fail("duplicate item name");
                expect("rect");
                for (auto& value : entry.rect) value = number();
                if (entry.rect[0] < 0 || entry.rect[1] < 0 || entry.rect[2] <= 0 || entry.rect[3] <= 0
                    || entry.rect[0] + entry.rect[2] > 640 || entry.rect[1] + entry.rect[3] > 480)
                    fail("rect must fit the 640x480 UI canvas");
                expect("text");
                entry.text = token();
                if (entry.text.empty() || entry.text.size() > 128) fail("invalid text length");
                expect("textscale");
                entry.scale = number();
                if (entry.scale <= 0 || entry.scale > 1) fail("invalid textscale");
                const auto kind = token();
                if (kind == "type")
                {
                    expect("ITEM_TYPE_BUTTON");
                    expect("action");
                    close_action();
                    entry.button = true;
                }
                else if (kind != "decoration") fail("expected decoration or button type");
                expect("}");
                result.items.push_back(std::move(entry));
            }
            if (result.items.empty()) fail("menu has no items");
            if (!token().empty()) fail("unexpected content after menu");
            return result;
        }

    private:
        const std::string& source_;
        std::size_t cursor_ = 0;

        [[noreturn]] void fail(const char* message) const
        {
            throw std::runtime_error(std::string(message) + " at byte " + std::to_string(cursor_));
        }

        std::string token()
        {
            while (cursor_ < source_.size())
            {
                const char ch = source_[cursor_];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { ++cursor_; continue; }
                if (source_.compare(cursor_, 2, "//") == 0)
                {
                    const auto end = source_.find('\n', cursor_);
                    cursor_ = end == std::string::npos ? source_.size() : end + 1;
                    continue;
                }
                break;
            }
            if (cursor_ == source_.size()) return {};
            const auto start = cursor_;
            const char ch = source_[cursor_++];
            if (ch == '#') fail("preprocessor directives are not supported by the diagnostic parser");
            if (ch == '{' || ch == '}' || ch == ';') return std::string(1, ch);
            if (ch == '"')
            {
                std::string value;
                while (cursor_ < source_.size())
                {
                    char next = source_[cursor_++];
                    if (next == '"') return value;
                    if (next == '\n' || next == '\r' || next == '\0') fail("invalid quoted text");
                    if (next == '\\')
                    {
                        if (cursor_ == source_.size()) fail("unfinished escape");
                        next = source_[cursor_++];
                        if (next != '"' && next != '\\') fail("unsupported escape");
                    }
                    value.push_back(next);
                }
                fail("unterminated string");
            }
            while (cursor_ < source_.size())
            {
                const char next = source_[cursor_];
                if (next == ' ' || next == '\t' || next == '\r' || next == '\n'
                    || next == '{' || next == '}' || next == ';') break;
                ++cursor_;
            }
            return source_.substr(start, cursor_ - start);
        }

        void expect(const char* expected)
        {
            if (token() != expected) fail((std::string("expected ") + expected).c_str());
        }

        float number()
        {
            const auto value = token();
            char* end = nullptr;
            const float result = std::strtof(value.c_str(), &end);
            if (value.empty() || *end || !std::isfinite(result)) fail("expected finite number");
            return result;
        }

        void close_action()
        {
            expect("{");
            expect("close");
            expect("disk_menu_test");
            expect(";");
            expect("}");
        }
    };
}
