#include <std_include.hpp>

#include <component/gamepad/controller/runtime.hpp>

#include <cassert>
#include <system_error>

#include <component/gamepad/controller/trace.hpp>
#include <component/gamepad/controller/calibration/normalize.hpp>
#include <component/gamepad/controller/engine/command.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace
      {
        // Where measured calibration profiles live. The game keeps player state under
        // players/, and a profile is player state: it describes one person's hardware.
        //
        path
        calibration_directory ()
        {
          std::error_code ec;
          const path here (std::filesystem::current_path (ec));

          return (ec ? path () : here) / "players" / "controller";
        }
      }

      runtime::
      runtime (bool developer)
        : developer_ (developer),
          ctx_ (sink_, developer),
          xinput_ (ctx_),
          devices_ (ctx_),
          discovery_ (ctx_, devices_, xinput_),
          drivers_ (ctx_, xinput_),
          calibration_ (ctx_, calibration_directory ()),
          keys_ (ctx_, dvars_),
          binds_ (ctx_, dvars_),
          view_ (ctx_, dvars_)
      {
        report (sink_, severity::info, facility::runtime, errc::none,
                "controller runtime initialized");
      }

      // Defined out of line so owned subsystems may be forward declared in the header
      // and completed only in this translation unit.
      //
      runtime::
      ~runtime () = default;

      context
      runtime::
      make_context () noexcept
      {
        return context (sink_, developer_);
      }

      void
      runtime::
      engine_ready ()
      {
        if (engine_ready_)
          return;

        dvars_ = engine::register_dvars (ctx_);
        engine::register_commands (ctx_, *this);

        // Take the bindings the configured layout implies before the first frame, so
        // a controller attached at startup is already bound when it is first polled.
        //
        binds_.apply_configured_layout ();

        // One eager pass so a controller present at startup is bound on frame one
        // rather than up to an interval later.
        //
        discovery_.scan_now ();

        engine_ready_ = true;
      }

      const calibration::profile&
      runtime::
      profile_for (controller::family f)
      {
        optional<calibration::profile>& slot (profiles_[static_cast<size_t> (f)]);

        if (!slot)
        {
          // A stored profile is validated by the store; a missing one is not an
          // error. The built-in default is the identity, so an uncalibrated device
          // passes through unchanged rather than being shaped by another device's
          // measurements.
          //
          optional<calibration::profile> stored (calibration_.load (f, nullopt));
          slot = stored ? move (*stored) : calibration::default_profile (f);
        }

        return *slot;
      }

      bool
      runtime::
      advance (driver::driver& d, const device_connection& dc, input_frame& out)
      {
        // Stack storage: the acquisition path allocates nothing.
        //
        raw_sample raw;
        canonical_sample canonical;

        const timestamp acquired (clock::now ());

        if (!d.poll (raw, canonical))
          return false;

        calibration::apply (profile_for (dc.identity.family), raw, canonical);

        out = input_frame {dc.id,
                           dc.identity.family,
                           dc.link,
                           ++sequence_,
                           latency_span {acquired, timestamp {}},
                           canonical};
        return true;
      }

      void
      runtime::
      apply_light_bar ()
      {
        // Only PlayStation-family devices have a light bar, and only when they report
        // the capability. Anything else has nothing to drive.
        //
        const bool ps (latest_.family == controller::family::dualshock4 ||
                       latest_.family == controller::family::dualsense ||
                       latest_.family == controller::family::dualsense_edge);

        if (!ps || !latest_.state.caps.has (capability::light_bar))
          return;

        const bool on (engine::read (dvars_.light_bar, true));

        // When disabled, forget any applied colour so re-enabling re-sends, but do
        // not fight whatever the OS or Steam set in the meantime.
        //
        if (!on)
        {
          lit_device_ = no_device;
          lit_colour_ = 0;
          return;
        }

        const auto clamp8 = [] (int v) -> uint8_t
        {
          return static_cast<uint8_t> (v < 0 ? 0 : (v > 255 ? 255 : v));
        };

        const uint8_t r (clamp8 (engine::read (dvars_.light_bar_r, 196)));
        const uint8_t g (clamp8 (engine::read (dvars_.light_bar_g, 151)));
        const uint8_t b (clamp8 (engine::read (dvars_.light_bar_b, 54)));

        const uint32_t colour (static_cast<uint32_t> (r) << 16 |
                               static_cast<uint32_t> (g) << 8 | b);

        // Re-send only when the device or the colour changed; a light bar holds its
        // colour, so writing it every frame would be pure output-report traffic.
        //
        if (active_ == lit_device_ && colour == lit_colour_)
          return;

        drivers_.submit (active_, driver::light_bar_request {r, g, b});

        lit_device_ = active_;
        lit_colour_ = colour;
      }

      void
      runtime::
      frame ()
      {
        CONTROLLER_ZONE ("controller::frame");

        // The hook is installed before the engine's dvar and command systems exist,
        // so the first frames before engine_ready () are a no-op rather than a read
        // of unregistered dvars.
        //
        if (!engine_ready_)
        {
          CONTROLLER_FRAME_MARK ();
          return;
        }

        discovery_.scan ();
        drivers_.reconcile (devices_);

        engine::publish_present (dvars_, drivers_.size () != 0);

        if (!engine::read (dvars_.enabled, true) || drivers_.size () == 0)
        {
          // Nothing will drive the keys this frame. Release whatever the controller
          // left held rather than leaving a '+' action running.
          //
          if (had_device_)
          {
            keys_.release_all ();
            view_.idle ();
            active_ = no_device;
            had_device_ = false;

            // Forget the applied light-bar colour so a device that returns is lit
            // again rather than assumed to still hold it.
            //
            lit_device_ = no_device;
            lit_colour_ = 0;
          }

          CONTROLLER_FRAME_MARK ();
          return;
        }

        // Poll every bound device, but follow exactly one: the engine's key state and
        // view are singular, so two devices driving them would fight. The device that
        // produced a reading this frame is the one in the player's hands, and the one
        // already being followed keeps precedence when several report at once.
        //
        input_frame candidate;
        bool have_candidate (false);

        drivers_.for_each ([this, &candidate, &have_candidate]
                           (driver::driver& d, const device_connection& dc)
        {
          input_frame f;

          if (!advance (d, dc, f))
            return;

          if (!have_candidate || dc.id == active_)
          {
            candidate = move (f);
            have_candidate = true;
          }
        });

        if (have_candidate)
        {
          latest_ = move (candidate);
          latest_.timing.consumed = clock::now ();

          // The sequence is drawn from one monotonically increasing counter, so a
          // published frame is always newer than the last. Consumers order frames by
          // it, which is only meaningful if it never repeats or goes backwards.
          //
          assert (latest_.sequence > last_published_);
          last_published_ = latest_.sequence;

          // A device that starts producing takes over from one that has stopped. The
          // outgoing device's keys are released first, so a button it was holding does
          // not stay latched under the new device's edge detection.
          //
          if (active_ != latest_.device)
            keys_.release_all ();

          active_ = latest_.device;
          had_device_ = true;

          keys_.dispatch (latest_.state);

          // Latch the sticks for the movement hook, which runs later in the frame
          // when the engine builds the usercmd. The view is not advanced here; there
          // is no command to write it onto yet.
          //
          view_.observe (latest_.state);

          apply_light_bar ();
        }
        else if (had_device_)
        {
          // XInput suppresses unchanged packets. The key layer still needs a frame
          // tick for held-button repeats and accelerated menu scrolling.
          keys_.dispatch (latest_.state);
        }

        // A device that produced no new reading this frame is deliberately not
        // idled: an unchanged reading is what a held stick looks like (XInput reports
        // the same packet number, and an HID pad the same bytes), so the latched
        // deflection must persist and keep driving the view. A stick returning to
        // centre is itself a state change and arrives as a fresh, centred sample.

        CONTROLLER_FRAME_MARK ();
      }
    }
  }
}
