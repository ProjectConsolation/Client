#include <std_include.hpp>

#include <component/gamepad/controller/transport/xinput-module.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace transport
      {
        xinput_module::
        xinput_module (const context& ctx)
        {
          load (ctx);
        }

        xinput_module::
        ~xinput_module ()
        {
          if (handle_ != nullptr)
            FreeLibrary (handle_);
        }

        void
        xinput_module::
        load (const context& ctx)
        {
          // Try the variants in descending order of capability. The reason each one
          // is in the list, and where it comes from, is what a future maintainer
          // needs to know before reordering or removing an entry:
          //
          //   xinput1_4.dll   In-box since Windows 8. The current runtime; preferred
          //                   because it is always the platform's own up-to-date copy
          //                   and needs no redistributable.
          //   xinput1_3.dll   The DirectX SDK (June 2010) redistributable. Present
          //                   only if some installer shipped it. We accept it as the
          //                   middle option because it exposes the same extended
          //                   entry point as 1.4, so behavior is identical when 1.4
          //                   is somehow absent but the redist is present.
          //   xinput9_1_0.dll The legacy in-box runtime present on every supported
          //                   Windows. It is the guaranteed fallback, but it does not
          //                   export the extended state entry point, so the guide
          //                   button is unavailable through it.
          //
          static constexpr const char* variants[]
          {
            "xinput1_4.dll",
            "xinput1_3.dll",
            "xinput9_1_0.dll",
          };

          for (const char* name: variants)
          {
            HMODULE h (LoadLibraryA (name));

            if (h == nullptr)
              continue;

            auto get_state (reinterpret_cast<get_state_fn> (
              GetProcAddress (h, "XInputGetState")));
            auto get_caps (reinterpret_cast<get_caps_fn> (
              GetProcAddress (h, "XInputGetCapabilities")));
            auto set_state (reinterpret_cast<set_state_fn> (
              GetProcAddress (h, "XInputSetState")));

            // Without these three the library is unusable; treat it as if it had not
            // loaded and try the next variant rather than half-initializing.
            //
            if (get_state == nullptr || get_caps == nullptr || set_state == nullptr)
            {
              FreeLibrary (h);
              continue;
            }

            // XInputGetStateEx is exported by ordinal 100 and has no public name. It
            // is identical to XInputGetState except that it does not clear the guide
            // button bit. Resolving it is best-effort: its absence only means the
            // guide button cannot be decoded, not that the runtime is unusable.
            //
            auto get_state_ex (reinterpret_cast<get_state_fn> (
              GetProcAddress (h, reinterpret_cast<const char*> (100))));

            handle_ = h;
            name_ = name;
            get_state_ = get_state;
            get_state_ex_ = get_state_ex;
            get_capabilities_ = get_caps;
            set_state_ = set_state;

            ctx.report (severity::info, facility::transport, errc::none,
                        string ("XInput loaded via ") + name +
                        (get_state_ex != nullptr
                         ? " (guide button available)"
                         : " (no guide button)"));
            return;
          }

          ctx.report (severity::warning, facility::transport, errc::transport_failure,
                      "no XInput runtime could be loaded; "
                      "XInput controllers will not be available");
        }

        DWORD
        xinput_module::
        get_state (DWORD user_index, XINPUT_STATE& out) const noexcept
        {
          if (get_state_ex_ != nullptr)
            return get_state_ex_ (user_index, &out);

          if (get_state_ != nullptr)
            return get_state_ (user_index, &out);

          return ERROR_DEVICE_NOT_CONNECTED;
        }

        DWORD
        xinput_module::
        get_capabilities (DWORD user_index,
                          DWORD flags,
                          XINPUT_CAPABILITIES& out) const noexcept
        {
          if (get_capabilities_ != nullptr)
            return get_capabilities_ (user_index, flags, &out);

          return ERROR_DEVICE_NOT_CONNECTED;
        }

        DWORD
        xinput_module::
        set_state (DWORD user_index, XINPUT_VIBRATION& vibration) const noexcept
        {
          if (set_state_ != nullptr)
            return set_state_ (user_index, &vibration);

          return ERROR_DEVICE_NOT_CONNECTED;
        }
      }
    }
  }
}
