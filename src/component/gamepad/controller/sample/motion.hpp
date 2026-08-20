#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // A three-component vector in the controller's own sensor frame.
      //
      // Deliberately its own type: sensor-frame vectors must not be assigned to or
      // from the screen-space and world-space vectors the aim layer uses. The axes
      // are the controller's physical axes, not the game's.
      //
      struct sensor_vec3
      {
        float x {0.0f};
        float y {0.0f};
        float z {0.0f};
      };

      // Gyroscope reading: angular velocity in radians per second.
      //
      // The unit is fixed here so every driver converts its device-specific raw
      // counts into the same physical quantity; a consumer never has to know which
      // family produced the sample.
      //
      struct gyro_sample
      {
        sensor_vec3 angular_velocity {};
      };

      // Accelerometer reading: proper acceleration in standard gravities
      // (1 g = 9.80665 m/s^2).
      //
      struct accel_sample
      {
        sensor_vec3 acceleration {};
      };

      // Combined motion reading for one sample.
      //
      // device_timestamp, when present, is the controller's own sensor clock as
      // reported in the HID payload. It is retained for consumers that integrate
      // motion over the device's timeline; it is not the subsystem clock and is
      // never used for latency.
      //
      struct motion_sample
      {
        gyro_sample        gyro {};
        accel_sample       accel {};
        optional<uint32_t> device_timestamp;
      };
    }
  }
}
