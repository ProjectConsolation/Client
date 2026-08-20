#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/mapping/key.hpp>
#include <component/gamepad/controller/mapping/logical.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace mapping
      {
        // The subsystem's controller binding table.
        //
        // Maps each engine controller key to the command it invokes. This is the
        // subsystem's own model of the controller bindings; the engine integration
        // synchronizes it with the engine's bind table. It is the meeting point of the
        // three layers Ã¢â‚¬â€ an engine key on one side, a command (a logical action's
        // string) on the other Ã¢â‚¬â€ and it keeps them explicit rather than letting a key
        // name imply a command. An empty command means the key is unbound.
        //
        class binding_table
        {
        public:
          void
          bind (engine_key, string command);

          // Bind a key to a logical action's command.
          //
          void
          bind (engine_key, action);

          void
          unbind (engine_key) noexcept;

          void
          clear () noexcept;

          // The command bound to a key, or nullptr when the key is unbound.
          //
          const string*
          command_for (engine_key) const noexcept;

          // Visit every bound key in engine-key order.
          //
          void
          for_each (function_ref<void (engine_key, const string&)>) const;

          size_t
          size () const noexcept;

        private:
          static constexpr size_t count {engine_key_count};

          // Indexed by mapping::key_index; an empty string is unbound.
          //
          array<string, count> commands_;
        };

        // Populate a table from a named button layout.
        //
        // Recovers the base controller layouts Ã¢â‚¬â€ gamepad_default and the tactical,
        // lefty, nomad, and _alt variants Ã¢â‚¬â€ as engine-key-to-command bindings. An
        // unrecognized name falls back to the default layout. The table is cleared
        // first, so the result depends only on the name.
        //
        void
        apply_button_layout (binding_table&, string_view name);
      }
    }
  }
}
