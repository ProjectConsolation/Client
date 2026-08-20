#include <std_include.hpp>

#include <component/gamepad/controller/diagnostic.hpp>

#include <component/engine/console/console.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      const char*
      to_string (facility f) noexcept
      {
        switch (f)
        {
        case facility::runtime:     return "runtime";
        case facility::discovery:   return "discovery";
        case facility::transport:   return "transport";
        case facility::driver:      return "driver";
        case facility::decode:      return "decode";
        case facility::sample:      return "sample";
        case facility::calibration: return "calibration";
        case facility::mapping:     return "mapping";
        case facility::aim:         return "aim";
        case facility::steam:       return "steam";
        case facility::engine:      return "engine";
        case facility::debug:       return "debug";
        }

        return "runtime";
      }

      const char*
      to_string (severity s) noexcept
      {
        switch (s)
        {
        case severity::info:    return "info";
        case severity::warning: return "warning";
        case severity::error:   return "error";
        }

        return "info";
      }

      void
      logging_sink::
      consume (const diagnostic& d)
      {
        // Compose one line with a fixed shape so a reader always finds the facility
        // first and the structured code and device last:
        //
        //   controller: <facility>: <message> [<code>][ <device>]
        //
        // The code is omitted when it carries no information (errc::none) and the
        // device is omitted when the record is not device-specific.
        //
        // The logger flushes a statement when the accumulator temporary is
        // destroyed, so the whole line must be appended to a single accumulator.
        // We seed one from the chosen severity and chain the remaining fields onto
        // it; it flushes once when emit returns.
        //
        string line = "controller: " + string (to_string (d.origin)) + ": " + d.message;
        if (d.code != errc::none) line += " [" + string (to_string (d.code)) + "]";
        if (d.device) line += " device=" + std::to_string (d.device.value ());
        line += '\n';

        switch (d.level)
        {
        case severity::info:    console::info ("%s", line.c_str ()); break;
        case severity::warning: console::warn ("%s", line.c_str ()); break;
        case severity::error:   console::error ("%s", line.c_str ()); break;
        }
      }

      void
      report (diagnostic_sink& sink,
              severity level,
              facility origin,
              errc code,
              device_id device,
              string message)
      {
        sink.consume (diagnostic {level, origin, code, device, move (message)});
      }
    }
  }
}
