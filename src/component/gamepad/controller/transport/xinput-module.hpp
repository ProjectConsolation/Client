#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>

#include <windows.h>
#include <xinput.h>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace transport
      {
        // Safe, explicit loader for the XInput runtime.
        //
        // XInput ships as several side-by-side DLLs with different capabilities, and
        // the project deliberately does not link an XInput import library: it resolves
        // the entry points by name at run time so a missing runtime degrades to "no
        // XInput" instead of failing to load the process. The variants are tried in
        // descending order of capability; the reason each one exists is recorded at
        // the load site.
        //
        // The module owns the loaded library and the resolved pointers for its
        // lifetime. It is non-copyable and non-movable because those pointers are
        // only valid while this instance keeps the library mapped.
        //
        class xinput_module
        {
        public:
          explicit
          xinput_module (const context&);

          ~xinput_module ();

          xinput_module (const xinput_module&) = delete;
          xinput_module& operator= (const xinput_module&) = delete;
          xinput_module (xinput_module&&) = delete;
          xinput_module& operator= (xinput_module&&) = delete;

          bool
          loaded () const noexcept {return handle_ != nullptr;}

          // Name of the variant that was loaded, or "none".
          //
          const char*
          dll_name () const noexcept {return name_;}

          // Whether the loaded variant exposes the guide (Xbox) button.
          //
          // The public XInputGetState masks the guide button out. The extended entry
          // point exported by ordinal 100 does not; when it is available get_state
          // uses it, so the driver can decode the guide button as a device fact.
          //
          bool
          has_guide_button () const noexcept {return get_state_ex_ != nullptr;}

          // Thin pass-throughs to the resolved entry points. Each returns an XInput
          // status code (ERROR_SUCCESS on success, ERROR_DEVICE_NOT_CONNECTED when
          // the slot is empty). When the runtime is not loaded they report the slot
          // as empty rather than touching a null pointer.
          //
          DWORD
          get_state (DWORD user_index, XINPUT_STATE& out) const noexcept;

          DWORD
          get_capabilities (DWORD user_index,
                            DWORD flags,
                            XINPUT_CAPABILITIES& out) const noexcept;

          DWORD
          set_state (DWORD user_index, XINPUT_VIBRATION& vibration) const noexcept;

        private:
          using get_state_fn = DWORD (WINAPI*) (DWORD, XINPUT_STATE*);
          using get_caps_fn  = DWORD (WINAPI*) (DWORD, DWORD, XINPUT_CAPABILITIES*);
          using set_state_fn = DWORD (WINAPI*) (DWORD, XINPUT_VIBRATION*);

          void
          load (const context&);

          HMODULE     handle_ {nullptr};
          const char* name_ {"none"};

          get_state_fn get_state_ {nullptr};
          get_state_fn get_state_ex_ {nullptr};  // Ordinal 100; includes guide button.
          get_caps_fn  get_capabilities_ {nullptr};
          set_state_fn set_state_ {nullptr};
        };
      }
    }
  }
}
