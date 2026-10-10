#include "../src/component/gamepad/controller/mapping/icon_text.hpp"
#include "../src/component/gamepad/controller/engine/input_activity.hpp"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    namespace icons = gamepad::unstable::controller::mapping::icon_text;
    assert(icons::override_index("auto") == -1);
    assert(icons::override_index("XbOx") == 0);
    assert(icons::override_index("PS3") == 1);
    assert(icons::override_index("ps3junk") == -1);
    for (std::size_t i = 0; i < icons::count; ++i)
    {
        const auto* token = icons::tokens[i].data();
        assert(std::strlen(token) == icons::token_size);
        assert(icons::decode(token) == static_cast<int>(i));
        for (std::size_t n = 0; n < icons::token_size; ++n)
            assert(icons::decode(std::string_view(token, n)) == -1);
        std::string malformed(token);
        malformed[7] = '!';
        assert(icons::decode(malformed) == -1);
        malformed = token;
        malformed[4] = 'X';
        assert(icons::decode(malformed) == -1);
    }
    assert(icons::advance(32) == 32);
    assert(icons::advance(24) == 24);
    gamepad::unstable::controller::engine::input_activity activity;
    std::array<float, 6> axes{0.5f, 0, 0, 0, 1, 0};
    assert(activity.analog(axes, true));
    activity.interrupt();
    assert(!activity.allow_repeat());
    assert(!activity.analog(axes, true));
    axes[0] = 0.52f;
    assert(!activity.analog(axes, true));
    axes[0] = 0.57f;
    assert(activity.analog(axes, true));
    assert(activity.allow_repeat());
    activity.interrupt();
    activity.controller_press();
    assert(activity.allow_repeat());
}
