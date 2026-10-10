#pragma once
namespace game
{
  // Stock protocol 47 encodes only four direction bits. Protocol 48 preserves
  // signed analog axes; peers must use the same codec, regardless of device.
  inline constexpr int consolation_protocol = 48;
}
