#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/engine/dvar.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        // Bridges the subsystem's binding model to the engine's bind table.
        //
        // The engine stores a binding as an index into a fixed command table, keyed by
        // keynum. The subsystem describes a layout as a command per engine key. This is
        // the only place the two meet: the mapping layer never talks to the engine, and
        // the engine's bind table is never read as though it were the subsystem's own
        // model. What the player has bound lives in the engine's table, which stays
        // authoritative.
        //
        class bind_bridge
        {
        public:
          bind_bridge (const context&, const dvars&);

          // Write a named button layout into the engine's bind table.
          //
          // An unrecognized name, including "custom", yields the default layout. This is
          // what the controls menu's reset relies on: it leaves gpad_buttonConfig alone
          // and expects the bindings to go back to the defaults.
          //
          void
          apply_layout (string_view name);

          // Write the layout named by gpad_buttonConfig.
          //
          // Used at startup, where a "custom" configuration means the player's own
          // bindings are about to be restored from their config and must not be
          // overwritten. Reset goes through apply_layout, not this.
          //
          void
          apply_configured_layout ();

          // Re-apply the layout gpad_buttonConfig names, whatever it is.
          //
          // This is what the controls menu's reset invokes, through the
          // bindgpbuttonsconfigs command, after it has reset the keyboard bindings.
          //
          void
          reapply_layout ();

          // Record that the player rebound a controller key, so no named layout claims
          // to describe the current bindings.
          //
          void
          note_manual_rebind () noexcept;

          // The keys a command is bound to, restricted to the half of the key space
          // that matches the active input source.
          //
          // The engine's own lookup walks all 256 keys and answers with whichever key
          // it meets first, so a command bound to both a mouse button and a controller
          // button answers with the mouse. The menus use the result to draw a prompt,
          // and the prompt must show the device the player is holding. keys_out
          // receives up to two keynums and is -1 padded; the count is returned.
          //
          static size_t
          command_keys (int client,
                        bool controller_in_use,
                        const char* command,
                        int (&keys_out)[2]) noexcept;

        private:
          // Rewrite any controller key still bound to a keyboard command that a
          // controller merges (+activate/+reload -> +usereload, +melee_breath ->
          // +holdbreath) onto the merged command. A "custom" configuration keeps the
          // player's own bindings, which may carry such a legacy bind from an older
          // build; leaving it in place makes the key do the wrong thing in-game (a raw
          // +activate picks up instead of reloading) and, because the controls menu looks
          // the reload/use row up by the merged command, draws it as unbound. The rewrite
          // is idempotent: a key already on the merged command is left untouched.
          //
          void
          migrate_controller_commands ();

          const context& ctx_;
          const dvars&   dvars_;
        };

        // The command a controller uses in place of a keyboard command.
        //
        // The controller merges activate and reload onto one button, and holds breath
        // with melee. A menu asking which key runs "+reload" is really asking which
        // button runs "+usereload" once a controller is in the player's hands.
        //
        const char*
        controller_command_for (const char* command) noexcept;
      }
    }
  }
}
