#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // Structured error condition recognized across the subsystem.
      //
      // A single enumeration backs both the throwing channel (error, below) and the
      // non-throwing reporting channel (diagnostic.hxx), so a failure has one
      // stable identity whether it aborts a cold operation or is merely reported.
      // The tokens returned by to_string are stable and machine-grep-able; treat
      // them as part of the diagnostic contract.
      //
      enum class errc : uint8_t
      {
        none,

        // Discovery and identity.
        //
        device_unavailable,   // The device went away before the operation ran.
        ambiguous_identity,   // Identity or transport could not be pinned down.
        unsupported_device,   // No native driver claims this device.

        // Transport.
        //
        transport_failure,    // An OS transport call failed.

        // Report decoding.
        //
        report_malformed,     // Report id/length/framing did not match the device.
        report_truncated,     // Fewer bytes than the decoded layout requires.
        checksum_mismatch,    // CRC/checksum did not validate.

        // Output.
        //
        output_rejected,      // A guarded output report was refused (see driver).

        // Calibration.
        //
        calibration_invalid,  // A calibration value failed validation.
        calibration_version,  // A stored profile has an unsupported version.

        // Aim.
        //
        graph_invalid,        // An aim graph failed knot validation.

        // Mapping.
        //
        binding_invalid,      // A binding could not be resolved.

        // Steam.
        //
        steam_unavailable,    // The Steamworks input runtime is not present.
        steam_unsuitable,     // Steam Input is present but cannot own the device.

        // Engine integration.
        //
        hook_failed,          // A detour or call patch could not be installed.
        dvar_registration,    // A dvar or command failed to register.
      };

      // Stable kebab-case token for a condition, suitable for logs and tests.
      //
      const char*
      to_string (errc) noexcept;

      ostream&
      operator<< (ostream&, errc);

      // Exception carrying a structured error condition.
      //
      // Raised only from cold paths: discovery, driver binding, file I/O, and
      // configuration. It must never propagate out of an engine hook or the
      // per-frame acquisition path. Those paths are crash-sensitive and report
      // through the diagnostic sink and degrade, rather than throwing across the
      // engine ABI boundary.
      //
      class error: public runtime_error
      {
      public:
        error (errc, const string& what);
        error (errc, const char* what);

        errc
        code () const noexcept {return code_;}

      private:
        errc code_;
      };
    }
  }
}
