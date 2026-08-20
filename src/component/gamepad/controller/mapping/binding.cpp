#include <std_include.hpp>

#include <component/gamepad/controller/mapping/binding.hpp>

#include <cctype>
#include <string>

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
          size_t
          index_of (engine_key k) noexcept
          {
            return key_index (k);
          }

          string
          lowercase (string_view s)
          {
            string r (s);
            for (char& c: r)
              c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
            return r;
          }

          bool
          contains (const string& s, string_view sub) noexcept
          {
            return s.find (sub) != string::npos;
          }
        }

        void
        binding_table::
        bind (engine_key k, string command)
        {
          commands_[index_of (k)] = move (command);
        }

        void
        binding_table::
        bind (engine_key k, action a)
        {
          commands_[index_of (k)] = command (a);
        }

        void
        binding_table::
        unbind (engine_key k) noexcept
        {
          commands_[index_of (k)].clear ();
        }

        void
        binding_table::
        clear () noexcept
        {
          for (string& c: commands_)
            c.clear ();
        }

        const string*
        binding_table::
        command_for (engine_key k) const noexcept
        {
          const string& c (commands_[index_of (k)]);
          return c.empty () ? nullptr : &c;
        }

        void
        binding_table::
        for_each (function_ref<void (engine_key, const string&)> fn) const
        {
          for (size_t i (0); i < count; ++i)
          {
            if (!commands_[i].empty ())
              fn (all_engine_keys[i], commands_[i]);
          }
        }

        size_t
        binding_table::
        size () const noexcept
        {
          size_t n (0);
          for (const string& c: commands_)
          {
            if (!c.empty ())
              ++n;
          }
          return n;
        }

        void
        apply_button_layout (binding_table& t, string_view name)
        {
          const string n (lowercase (name));

          const bool tactical (contains (n, "tactical"));
          const bool lefty (contains (n, "lefty"));
          const bool nomad (contains (n, "nomad"));
          const bool alt (contains (n, "_alt"));

          t.clear ();

          // Shared bindings across every layout.
          //
          t.bind (engine_key::button_start, action::menu);
          t.bind (engine_key::button_back,  action::scoreboard);
          t.bind (engine_key::button_x,     action::use_reload);
          t.bind (engine_key::button_y,     action::next_weapon);
          t.bind (engine_key::dpad_up,      action::action_slot_1);
          t.bind (engine_key::dpad_down,    action::action_slot_2);
          t.bind (engine_key::dpad_left,    action::action_slot_3);
          t.bind (engine_key::dpad_right,   action::action_slot_4);
          t.bind (engine_key::button_a,     action::jump_stand);

          // The tactical layout swaps melee onto B and stance onto the right stick.
          //
          t.bind (engine_key::button_b,      tactical ? action::melee : action::stance);
          t.bind (engine_key::button_rstick, tactical ? action::stance : action::melee);

          if (lefty)
          {
            t.bind (engine_key::button_ltrig,  action::fire);
            t.bind (engine_key::button_rtrig,  action::ads);
            t.bind (engine_key::button_lshldr, nomad ? action::sprint : action::frag);
            t.bind (engine_key::button_rshldr, action::special_grenade);
            t.bind (engine_key::button_rstick, action::sprint);
          }
          else
          {
            t.bind (engine_key::button_rtrig,  action::fire);
            t.bind (engine_key::button_ltrig,  action::ads);
            t.bind (engine_key::button_rshldr,
                    alt ? action::special_grenade : action::frag);
            t.bind (engine_key::button_lshldr,
                    alt ? action::frag : action::special_grenade);
            t.bind (engine_key::button_lstick,
                    nomad ? action::jump_stand : action::sprint);
          }
        }
      }
    }
  }
}
