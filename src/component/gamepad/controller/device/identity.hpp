#pragma once

#include "component/gamepad/controller/support/types.hpp"
#include "component/gamepad/controller/support/utility.hpp"

namespace gamepad
{
  namespace unstable
  {
    namespace controller
    {
      // USB vendor identifier.
      //
      class vendor_id
      {
      public:
        constexpr explicit
        vendor_id (uint16_t v) noexcept: value_ (v) {}

        constexpr uint16_t
        value () const noexcept {return value_;}

        friend constexpr bool
        operator== (vendor_id, vendor_id) noexcept = default;

      private:
        uint16_t value_;
      };

      // USB product identifier.
      //
      class product_id
      {
      public:
        constexpr explicit
        product_id (uint16_t v) noexcept: value_ (v) {}

        constexpr uint16_t
        value () const noexcept {return value_;}

        friend constexpr bool
        operator== (product_id, product_id) noexcept = default;

      private:
        uint16_t value_;
      };

      // Controller family the subsystem drives natively.
      //
      // The family is the decoding contract: it selects which driver owns the
      // device and which native report model applies. It is intentionally not a
      // marketing label. "xbox" means "speaks the XInput state model"; the three
      // PlayStation families each decode a distinct HID report set and must not be
      // collapsed into one another (a DualSense Edge carries state a DualSense
      // decoder cannot represent).
      //
      enum class family : uint8_t
      {
        unknown,
        xbox,            // XInput-class controller.
        dualshock4,      // Sony DualShock 4 (CUH-ZCT1/ZCT2).
        dualsense,       // Sony DualSense (CFI-ZCT1).
        dualsense_edge,  // Sony DualSense Edge (CFI-ZER1).
      };

      const char*
      to_string (family) noexcept;

      ostream&
      operator<< (ostream&, family);

      // Well-known USB identifiers.
      //
      // Sourced from the Linux kernel HID driver drivers/hid/hid-playstation.c and
      // drivers/hid/hid-ids.h, which the PlayStation drivers in this subsystem are
      // designed against. Keep these next to the classifier that consumes them so a
      // maintainer can check an id against the kernel without leaving the file.
      //
      inline constexpr vendor_id vendor_sony {0x054C};
      inline constexpr vendor_id vendor_microsoft {0x045E};

      inline constexpr product_id product_ds4_gen1 {0x05C4};  // CUH-ZCT1 controller.
      inline constexpr product_id product_ds4_gen2 {0x09CC};  // CUH-ZCT2 controller.
      inline constexpr product_id product_ds4_dongle {0x0BA0}; // USB wireless adaptor.
      inline constexpr product_id product_dualsense {0x0CE6}; // CFI-ZCT1 controller.
      inline constexpr product_id product_dualsense_edge {0x0DF2}; // CFI-ZER1.

      // What a device is, independent of how it is currently connected.
      //
      // Connection and transport live on the device's connection record, not here:
      // the same identity can be reached over USB or Bluetooth without becoming a
      // different device.
      //
      struct device_identity
      {
        controller::family family {family::unknown};
        optional<vendor_id> vendor;
        optional<product_id> product;

        // Device release in binary-coded decimal from the USB descriptor, when the
        // transport exposes it. Used only to disambiguate hardware revisions; it is
        // never required for correct decoding.
        //
        optional<uint16_t> release;
      };

      // Classify a HID device by its USB identifiers.
      //
      // Returns family::unknown for anything the subsystem has no native driver
      // for. A caller must treat unknown as "do not decode": guessing a report
      // layout from an unrecognized device is exactly the ambiguity the driver
      // layer refuses to act on.
      //
      family
      classify (vendor_id, product_id) noexcept;
    }
  }
}
