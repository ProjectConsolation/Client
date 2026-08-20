#include <std_include.hpp>

#include <component/gamepad/controller/calibration/store.hpp>

#include <component/gamepad/controller/calibration/validate.hpp>

#include <fstream>
#include <format>
#include <string>
#include <system_error>

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      namespace calibration
      {
        namespace
        {
          constexpr const char* magic {"gamepad-controller-calibration"};

          // The highest family enumerator, so a family read from a file can be range
          // checked before it is cast back.
          //
          constexpr int family_max {
            static_cast<int> (controller::family::dualsense_edge)};

          constexpr int source_max {static_cast<int> (value_source::user)};
        }

        store::
        store (const context& ctx, path directory)
          : ctx_ (ctx), dir_ (move (directory))
        {
        }

        path
        store::
        file_for (controller::family family, optional<uint64_t> device_key) const
        {
          // Encode the family and, when present, the device key into the name so two
          // devices, or two families, never share a file.
          //
          string name (device_key
            ? std::format ("controller-{}-{:016x}.cal",
                           to_string (family), *device_key)
            : std::format ("controller-{}.cal", to_string (family)));

          return dir_ / name;
        }

        bool
        store::
        save (const profile& p) const
        {
          string why;
          if (!validate (p, why))
          {
            ctx_.report (severity::warning, facility::calibration,
                         errc::calibration_invalid,
                         "refusing to save an invalid calibration profile: " + why);
            return false;
          }

          std::error_code ec;
          filesystem::create_directories (dir_, ec);  // Best effort; open reports.

          const path file (file_for (p.family, p.device_key));
          ofstream os (file, ios::trunc);
          if (!os)
          {
            ctx_.report (severity::warning, facility::calibration,
                         errc::transport_failure,
                         "could not open calibration profile for writing");
            return false;
          }

          // Emit enough significant digits that a float survives the write/read round
          // trip exactly (max_digits10 for float is 9).
          //
          os.precision (9);

          os << magic << ' ' << p.version << '\n';
          os << "family " << static_cast<int> (p.family) << '\n';
          os << "source " << static_cast<int> (p.source) << '\n';
          os << "device_key " << (p.device_key ? 1 : 0) << ' '
             << (p.device_key ? *p.device_key : uint64_t {0}) << '\n';

          for (size_t i (0); i < stick_count; ++i)
          {
            const stick_calibration& s (p.sticks[i]);
            os << "stick " << i << ' '
               << s.center_x << ' ' << s.center_y << ' '
               << s.range_x << ' ' << s.range_y << ' '
               << s.drift_threshold << '\n';
          }

          for (size_t i (0); i < trigger_count; ++i)
          {
            const trigger_calibration& t (p.triggers[i]);
            os << "trigger " << i << ' ' << t.min << ' ' << t.max << '\n';
          }

          os << "motion "
             << p.motion.gyro_bias.x << ' ' << p.motion.gyro_bias.y << ' '
             << p.motion.gyro_bias.z << ' '
             << p.motion.accel_bias.x << ' ' << p.motion.accel_bias.y << ' '
             << p.motion.accel_bias.z << ' '
             << p.motion.gyro_scale << ' ' << p.motion.accel_scale << '\n';

          os << "smoothing " << p.smoothing << '\n';

          return static_cast<bool> (os);
        }

        optional<profile>
        store::
        load (controller::family family, optional<uint64_t> device_key) const
        {
          const path file (file_for (family, device_key));

          ifstream is (file);
          if (!is)
            return nullopt;  // No stored profile is not an error.

          auto reject = [this] (const char* why) -> optional<profile>
          {
            ctx_.report (severity::warning, facility::calibration,
                         errc::calibration_invalid,
                         string ("calibration profile rejected: ") + why);
            return nullopt;
          };

          // Header: magic and version. An unsupported version is rejected rather than
          // reinterpreted under the current layout.
          //
          string token;
          unsigned version (0);
          if (!(is >> token >> version) || token != magic)
            return reject ("bad header");

          if (version == 0 || version > profile::current_version)
            return reject ("unsupported version");

          profile p;
          p.version = static_cast<uint16_t> (version);

          int family_i (0);
          int source_i (0);
          int has_key (0);
          uint64_t key_value (0);

          if (!(is >> token >> family_i) || token != "family" ||
              family_i < 0 || family_i > family_max)
            return reject ("bad family");

          if (!(is >> token >> source_i) || token != "source" ||
              source_i < 0 || source_i > source_max)
            return reject ("bad source");

          if (!(is >> token >> has_key >> key_value) || token != "device_key")
            return reject ("bad device key");

          p.family = static_cast<controller::family> (family_i);
          p.source = static_cast<value_source> (source_i);
          if (has_key != 0)
            p.device_key = key_value;

          for (size_t i (0); i < stick_count; ++i)
          {
            size_t idx (0);
            stick_calibration s;
            if (!(is >> token >> idx >> s.center_x >> s.center_y >>
                  s.range_x >> s.range_y >> s.drift_threshold) ||
                token != "stick" || idx != i)
              return reject ("bad stick record");

            p.sticks[i] = s;
          }

          for (size_t i (0); i < trigger_count; ++i)
          {
            size_t idx (0);
            trigger_calibration t;
            if (!(is >> token >> idx >> t.min >> t.max) ||
                token != "trigger" || idx != i)
              return reject ("bad trigger record");

            p.triggers[i] = t;
          }

          if (!(is >> token >> p.motion.gyro_bias.x >> p.motion.gyro_bias.y >>
                p.motion.gyro_bias.z >> p.motion.accel_bias.x >>
                p.motion.accel_bias.y >> p.motion.accel_bias.z >>
                p.motion.gyro_scale >> p.motion.accel_scale) ||
              token != "motion")
            return reject ("bad motion record");

          if (!(is >> token >> p.smoothing) || token != "smoothing")
            return reject ("bad smoothing record");

          // The file must describe the device it was requested for; a mismatch means
          // the wrong file or a corrupt one, and applying it would cross devices.
          //
          if (p.family != family || p.device_key != device_key)
            return reject ("family or device key mismatch");

          string why;
          if (!validate (p, why))
            return reject (why.c_str ());

          return p;
        }
      }
    }
  }
}
