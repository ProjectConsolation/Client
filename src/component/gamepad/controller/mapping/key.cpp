#include <std_include.hpp>

#include <component/gamepad/controller/mapping/key.hpp>

#include <cctype>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        namespace
        {
          struct named_key
          {
            engine_key  key;
            const char* name;
          };

          // The controller key names, in key-number order.
          //
          // The base engine has no names for these keys at all Ã¢â‚¬â€ its own tables stop at
          // 0xDE Ã¢â‚¬â€ so the subsystem supplies them, and Key_KeynumToString and
          // Key_StringToKeynum are detoured to consult this table. BUTTON_A..APAD_RIGHT
          // keep the names the console titles use, so a configuration written by any
          // build reads back the same. RSTICK_* are new, for the right stick keys the
          // base engine never distinguished.
          //
          constexpr named_key key_names[]
          {
            {engine_key::button_a,      "BUTTON_A"},
            {engine_key::button_b,      "BUTTON_B"},
            {engine_key::button_x,      "BUTTON_X"},
            {engine_key::button_y,      "BUTTON_Y"},
            {engine_key::button_lshldr, "BUTTON_LSHLDR"},
            {engine_key::button_rshldr, "BUTTON_RSHLDR"},
            {engine_key::button_start,  "BUTTON_START"},
            {engine_key::button_back,   "BUTTON_BACK"},
            {engine_key::button_lstick, "BUTTON_LSTICK"},
            {engine_key::button_rstick, "BUTTON_RSTICK"},
            {engine_key::button_ltrig,  "BUTTON_LTRIG"},
            {engine_key::button_rtrig,  "BUTTON_RTRIG"},
            {engine_key::dpad_up,       "DPAD_UP"},
            {engine_key::dpad_down,     "DPAD_DOWN"},
            {engine_key::dpad_left,     "DPAD_LEFT"},
            {engine_key::dpad_right,    "DPAD_RIGHT"},
            {engine_key::apad_up,       "APAD_UP"},
            {engine_key::apad_down,     "APAD_DOWN"},
            {engine_key::apad_left,     "APAD_LEFT"},
            {engine_key::apad_right,    "APAD_RIGHT"},
            {engine_key::rstick_up,     "RSTICK_UP"},
            {engine_key::rstick_down,   "RSTICK_DOWN"},
            {engine_key::rstick_left,   "RSTICK_LEFT"},
            {engine_key::rstick_right,  "RSTICK_RIGHT"},
          };

          static_assert (sizeof (key_names) / sizeof (key_names[0]) == engine_key_count,
                         "the key name table must cover every controller key");

          bool
          iequals (string_view a, string_view b) noexcept
          {
            if (a.size () != b.size ())
              return false;

            for (size_t i (0); i < a.size (); ++i)
            {
              if (std::tolower (static_cast<unsigned char> (a[i])) !=
                  std::tolower (static_cast<unsigned char> (b[i])))
                return false;
            }

            return true;
          }
        }

        bool
        is_controller_key (int keynum) noexcept
        {
          return key_from_keynum (keynum).has_value ();
        }

        size_t
        key_index (engine_key key) noexcept
        {
          for (size_t i = 0; i < all_engine_keys.size (); ++i)
            if (all_engine_keys[i] == key) return i;
          return 0;
        }

        const char*
        key_name (engine_key k) noexcept
        {
          const size_t i (key_index (k));

          // The enum only holds the twenty controller keys, so the index is always in
          // range; the invariant is asserted by the table's static_assert above.
          //
          return key_names[i].name;
        }

        optional<engine_key>
        key_from_name (string_view name) noexcept
        {
          for (const named_key& e: key_names)
          {
            if (iequals (name, e.name))
              return e.key;
          }

          return nullopt;
        }

        optional<engine_key>
        key_from_keynum (int keynum) noexcept
        {
          for (engine_key key: all_engine_keys)
            if (static_cast<int> (key) == keynum) return key;
          return nullopt;
        }
      }
    }
  }
}
