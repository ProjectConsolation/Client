#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/error.hpp>
#include <component/gamepad/controller/device/id.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Area of the subsystem a diagnostic originates from.
      //
      // The facility is the coarse locator a reader scans for first, so it names a
      // stage of the pipeline rather than a source file. It stays meaningful even
      // when the file layout changes.
      //
      enum class facility : uint8_t
      {
        runtime,
        discovery,
        transport,
        driver,
        decode,
        sample,
        calibration,
        mapping,
        aim,
        steam,
        engine,
        debug,
      };

      const char*
      to_string (facility) noexcept;

      // Severity of a structured diagnostic.
      //
      // Kept separate from the logger's level enum so the subsystem owns its own
      // escalation policy; the logging sink maps these onto log levels.
      //
      enum class severity : uint8_t
      {
        info,
        warning,
        error,
      };

      const char*
      to_string (severity) noexcept;

      // One structured diagnostic record.
      //
      // The four typed fields (level, origin, code, device) let a reader or a test
      // triage a failure without parsing prose, while message carries the specific
      // detail. device is no_device for conditions that are not tied to one
      // physical device.
      //
      struct diagnostic
      {
        severity  level {severity::info};
        facility  origin {facility::runtime};
        errc      code {errc::none};
        device_id device {};
        string    message;
      };

      // Consumer of structured diagnostics.
      //
      // Dynamic dispatch is justified here because diagnostics are off the hot path
      // and a sink lets the debug tooling and tests observe exactly the records the
      // log observes. The runtime owns one concrete sink for the whole subsystem.
      //
      class diagnostic_sink
      {
      public:
        virtual
        ~diagnostic_sink () = default;

        virtual void
        consume (const diagnostic&) = 0;
      };

      // Default sink: formats each record to the logger in a stable shape.
      //
      class logging_sink: public diagnostic_sink
      {
      public:
        void
        consume (const diagnostic&) override;
      };

      // Build a record and hand it to a sink in one call.
      //
      void
      report (diagnostic_sink&,
              severity,
              facility,
              errc,
              device_id,
              string message);

      // Report a condition not tied to a specific device.
      //
      inline void
      report (diagnostic_sink& sink,
              severity level,
              facility origin,
              errc code,
              string message)
      {
        report (sink, level, origin, code, no_device, move (message));
      }
    }
  }
}
