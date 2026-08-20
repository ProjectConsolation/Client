#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/engine/dvar.hpp>
#include <component/gamepad/controller/mapping/key.hpp>
#include <component/gamepad/controller/sample/sample.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace engine
      {
        // The local client the controller drives.
        //
        // The engine indexes its key state, catchers, and menus by local client. IW4
        // has no split screen on PC, so there is exactly one and it is zero; naming it
        // keeps the constant from reading as an arbitrary index at each call site.
        //
        inline constexpr int local_client {0};

        // Turns canonical samples into engine key events.
        //
        // The engine's own CL_KeyEvent is the specification this follows: for each
        // controller key that changed, update playerKeys, then route the event through
        // the location-selection catcher, the scoreboard, the UI catcher, and finally
        // the binding dispatcher, in that order. The subsystem synthesizes the events
        // rather than calling CL_KeyEvent because CL_KeyEvent also decides the input
        // source, and a controller key must not be mistaken for a keyboard press.
        //
        // Two device-specific behaviors are folded in here because they are properties
        // of a controller, not of a key:
        //
        //   - A stick pushed past a threshold produces a direction key: APAD_* for the
        //     left stick, RSTICK_* for the right. It does so only while a menu or the
        //     scoreboard is up; in gameplay a stick is a view or movement axis, and the
        //     keys are released so a held stick cannot leak a menu press into play.
        //
        //   - The d-pad and the triggers repeat while held, which is what drives menu
        //     scrolling. The repeat interval accelerates the longer a d-pad direction
        //     is held, matching the engine's own menu feel.
        //
        // The dispatcher owns the edge-detection state, so it must see every sample
        // for the device it is following, and must be told when that device goes away
        // (release_all) so no key is left latched down.
        //
        class key_dispatcher
        {
        public:
          // The dvars are referenced, not copied: their handles are null until the
          // first engine frame registers them, and every read carries the default that
          // registration will install.
          //
          key_dispatcher (const context&, const dvars&);

          // Emit the engine key events implied by moving from the previous sample to
          // this one. Must not throw: it is called from inside an engine detour.
          //
          void
          dispatch (const canonical_sample&) noexcept;

          // Release every controller key that is still held.
          //
          // Each release travels the same path a physical release does, so a '+'
          // action started by a key that is still down when its device disappears is
          // properly ended rather than left running.
          //
          void
          release_all () noexcept;

          // Whether the controller is the active input source.
          //
          // The engine draws controller glyphs and takes controller movement only
          // while this is true. It becomes true on any controller input and false as
          // soon as the player touches the keyboard or moves the mouse; the engine
          // detours report those through note_other_input ().
          //
          bool
          in_use () const noexcept {return in_use_;}

          void
          note_other_input () noexcept;

        private:
          // The engine distinguishes a first press from a repeat and from a release;
          // repeats are what make a held d-pad scroll a menu.
          //
          enum class key_event : uint8_t
          {
            pressed,
            repeated,
            released,
          };

          void
          set_in_use (bool) noexcept;

          // Keep g_useholdtime in step with the input source. The combined +usereload
          // button -- which only a controller carries -- reloads on a tap and uses on a
          // hold only while g_useholdtime is positive; the engine default of 0 makes
          // every press use (pick up) at once, which is right for the keyboard's separate
          // instant +activate but wrong for the controller. Applied every frame the
          // controller is sampled, so it survives the game registering the dvar after the
          // controller is already in use, and on a transition to the keyboard.
          //
          void
          apply_use_hold_time () noexcept;

          // Emit an event for a physical button: marks the controller as the input
          // source and refreshes the menu scroll timer before dispatching.
          //
          void
          emit_button (mapping::engine_key, key_event, unsigned time) noexcept;

          // Emit an event for any controller key. This is the CL_KeyEvent equivalent.
          //
          void
          emit (mapping::engine_key, key_event, unsigned time) noexcept;

          // Emit the stick-as-direction keys. Reads the latched deflection state rather
          // than the sample, so a stick already held when a menu opens reads as held.
          //
          void
          dispatch_apad (unsigned time) noexcept;

          void
          dispatch_buttons (const canonical_sample&, unsigned time) noexcept;

          // Whether a repeat should be swallowed. Menu scroll keys are let through on
          // their own schedule; everything else repeats only on the first press.
          //
          bool
          ignore_repeat (mapping::engine_key, int repeats, unsigned time) noexcept;

          void
          reset_scroll (mapping::engine_key, bool down, unsigned time) noexcept;

          void
          menu_key_event (mapping::engine_key, bool down) noexcept;

          bool
          scoreboard_key_event (mapping::engine_key) noexcept;

          const context& ctx_;
          const dvars&   dvars_;

          bool in_use_ {false};

          // Resolved on first use: g_useholdtime is a game dvar that does not exist until
          // a session is running.
          //
          dvar_t* use_hold_time_ {nullptr};

          // The stick deadzone dvars are user configuration and are validated on every
          // read. A rejection is reported once rather than on every frame.
          //
          bool reported_deadzone_ {false};

          // Edge-detection state. buttons_ is the set observed on the previous sample.
          //
          button_set buttons_;

          // Per stick axis, whether the negative and positive ends read as deflected.
          // Ordered right x, right y, left x, left y. Each stick drives its own four
          // keys, so the order only fixes the sequence events are emitted in.
          //
          static constexpr size_t axis_count {4};

          array<array<bool, 2>, axis_count> deflected_ {};
          array<array<bool, 2>, axis_count> was_deflected_ {};

          // Menu scroll repeat schedule, in engine milliseconds.
          //
          unsigned next_scroll_ {0};
          unsigned scroll_hold_start_ {0};
          optional<mapping::engine_key> scroll_hold_key_;
        };
      }
    }
  }
}
