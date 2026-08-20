#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        // A logical game action.
        //
        // An action is what a control means to the game, independent of the physical
        // button or engine key that produces it. Each action names the engine command
        // it invokes; a binding connects an engine key to that command. Keeping
        // actions separate from keys is what lets a control be rebound, or drawn with
        // a different glyph, without changing what it does.
        //
        enum class action : uint8_t
        {
          none,

          fire,             // +attack
          ads,              // +speed_throw (aim down sight)
          jump_stand,       // +gostand
          stance,           // +stance
          melee,            // +melee
          use_reload,       // +usereload
          sprint,           // +breath_sprint
          next_weapon,      // weapnext
          frag,             // +frag
          special_grenade,  // +smoke
          menu,             // togglemenu

          // +scores, not togglescores: the engine's bindable-command table has no
          // togglescores entry, so a key bound to it resolves to the unbound index and
          // is silently dropped. +scores is what the keyboard's TAB is bound to, and it
          // shows the scoreboard while held.
          //
          scoreboard,       // +scores
          action_slot_1,    // +actionslot 1
          action_slot_2,    // +actionslot 2
          action_slot_3,    // +actionslot 3
          action_slot_4,    // +actionslot 4
        };

        const char*
        to_string (action) noexcept;

        // The engine command an action invokes.
        //
        // Returns the command string ("+attack", "weapnext", ...) or an empty string
        // for action::none. This is the one place the logical action is tied to a
        // concrete engine command, so a binding stores a command while callers reason
        // in actions.
        //
        const char*
        command (action) noexcept;
      }
    }
  }
}
