#include <std_include.hpp>

#include <component/gamepad/controller/transport/device-notify.hpp>

#include <windows.h>
#include <dbt.h>
#include <hidsdi.h>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace transport
      {
        namespace
        {
          constexpr wchar_t window_class[] {L"gamepad_controller_devnotify"};
          // IW4x Controller/Transport/DeviceNotify: allow HID interfaces to settle,
          // then retry for Bluetooth devices whose first enumeration is premature.
          constexpr UINT_PTR rescan_timer {1};
          constexpr UINT_PTR retry_timer {2};

          // The pending flag lives on the window, reached through its user data, so the
          // window procedure has no global mutable state of its own. It is the same
          // atomic device_notifier::consume reads.
          //
          std::atomic<bool>*
          flag_of (HWND w) noexcept
          {
            return reinterpret_cast<std::atomic<bool>*> (
              GetWindowLongPtrW (w, GWLP_USERDATA));
          }

          LRESULT CALLBACK
          window_proc (HWND w, UINT msg, WPARAM wp, LPARAM lp) noexcept
          {
            switch (msg)
            {
            case WM_DEVICECHANGE:
              {
                if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
                {
                  const auto* change = reinterpret_cast<const DEV_BROADCAST_HDR*> (lp);
                  if (change && change->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
                  {
                    const auto first = SetTimer (w, rescan_timer, 300, nullptr);
                    const auto retry = SetTimer (w, retry_timer, 2000, nullptr);
                    if (first == 0 || retry == 0)
                      if (auto* f = flag_of (w))
                        f->store (true, std::memory_order_release);
                  }
                }

                return TRUE;
              }

            case WM_TIMER:
              if (wp == rescan_timer || wp == retry_timer)
              {
                KillTimer (w, wp);
                if (auto* f = flag_of (w))
                  f->store (true, std::memory_order_release);
                return 0;
              }
              break;

            case WM_CLOSE:
              DestroyWindow (w);
              return 0;

            case WM_DESTROY:
              PostQuitMessage (0);
              return 0;
            }

            return DefWindowProcW (w, msg, wp, lp);
          }
        }

        device_notifier::
        device_notifier (const context& ctx)
        {
          thread_ = std::thread ([this, ctx] () {run (ctx);});
        }

        device_notifier::
        ~device_notifier ()
        {
          // Sequentially consistent publication/check pairs ensure either this
          // thread sees the window or its creator sees the stop request. Without
          // that handshake, destruction before CreateWindow could join forever.
          stopping_.store (true);
          if (HWND w = static_cast<HWND> (window_.load ()))
            PostMessageW (w, WM_CLOSE, 0, 0);

          if (thread_.joinable ())
            thread_.join ();
        }

        void
        device_notifier::
        run (const context& ctx) noexcept
        {
          HINSTANCE instance (GetModuleHandleW (nullptr));

          WNDCLASSEXW wc {};
          wc.cbSize = sizeof (wc);
          wc.lpfnWndProc = &window_proc;
          wc.hInstance = instance;
          wc.lpszClassName = window_class;

          // The class may already be registered from a previous load; that is not an
          // error. Any other failure leaves discovery on its poll fallback.
          //
          if (RegisterClassExW (&wc) == 0 &&
              GetLastError () != ERROR_CLASS_ALREADY_EXISTS)
          {
            ctx.report (severity::warning, facility::discovery, errc::transport_failure,
                        "device-change window class registration failed; discovery "
                        "will poll");
            return;
          }

          HWND window (CreateWindowExW (0, window_class, window_class, 0,
                                       0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, instance, nullptr));

          if (window == nullptr)
          {
            ctx.report (severity::warning, facility::discovery, errc::transport_failure,
                        "device-change window creation failed; discovery will poll");
            return;
          }

          SetWindowLongPtrW (window, GWLP_USERDATA,
                             reinterpret_cast<LONG_PTR> (&pending_));
          window_.store (window);
          if (stopping_.load ())
          {
            DestroyWindow (window);
            window_.store (nullptr);
            return;
          }

          // Register for HID interface notifications specifically. The same GUID the
          // HID enumeration uses; only devices of this class raise the flag, so a
          // storage volume or a monitor mode-set does not wake discovery.
          //
          GUID hid_guid {};
          HidD_GetHidGuid (&hid_guid);

          DEV_BROADCAST_DEVICEINTERFACE_W filter {};
          filter.dbcc_size = sizeof (filter);
          filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
          filter.dbcc_classguid = hid_guid;

          HDEVNOTIFY notify (
            RegisterDeviceNotificationW (window, &filter,
                                         DEVICE_NOTIFY_WINDOW_HANDLE));

          if (notify == nullptr)
            ctx.report (severity::warning, facility::discovery, errc::transport_failure,
                        "HID device-change registration failed; discovery will poll");

          // Pump until the window is closed. GetMessageW blocks, so this thread spends
          // its life asleep and wakes only on a device change or shutdown.
          //
          MSG m;
          while (GetMessageW (&m, nullptr, 0, 0) > 0)
          {
            TranslateMessage (&m);
            DispatchMessageW (&m);
          }

          if (notify != nullptr)
            UnregisterDeviceNotification (notify);

          window_.store (nullptr, std::memory_order_release);
        }
      }
    }
  }
}
