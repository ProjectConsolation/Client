#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

#include <component/gamepad/controller/context.hpp>
#include <component/gamepad/controller/calibration/profile.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace calibration
      {
        // Persists calibration profiles to disk, one per file.
        //
        // A profile is stored in a small versioned text file under the store's
        // directory, named by family and, when available, device key. Because the file
        // name encodes both, and load () additionally checks that the file's own
        // family and key match what was asked for, a profile is never applied to an
        // unrelated device Ã¢â‚¬â€ the invariant the spec calls out. Loading validates the
        // profile and rejects an unsupported version or malformed content, reporting
        // the reason; a missing profile is not an error and simply yields nullopt so
        // the caller uses the default.
        //
        class store
        {
        public:
          store (const context&, path directory);

          optional<profile>
          load (controller::family, optional<uint64_t> device_key) const;

          bool
          save (const profile&) const;

          const path&
          directory () const noexcept {return dir_;}

        private:
          path
          file_for (controller::family, optional<uint64_t> device_key) const;

          const context& ctx_;
          path            dir_;
        };
      }
    }
  }
}
