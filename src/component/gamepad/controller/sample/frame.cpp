#include <std_include.hpp>

#include <component/gamepad/controller/sample/frame.hpp>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      ostream&
      operator<< (ostream& os, const input_frame& f)
      {
        // A compact one-line summary for diagnostics and the debug tooling: which
        // device and family, the sequence number, and the measured latency in
        // milliseconds when the frame has been consumed.
        //
        os << f.device << ' ' << f.family << " #" << f.sequence;

        if (f.timing.consumed != timestamp {})
        {
          auto ms (chrono::duration_cast<milliseconds> (f.timing.latency ()));
          os << ' ' << ms.count () << "ms";
        }

        return os;
      }
    }
  }
}
