#pragma once
#include <array>
#include <cstdint>

namespace gamepad::unstable::controller::engine
{
  // PC takeover is exclusive: stick drift, held triggers, repeats, and hotplug
  // cannot restore controller aiming. Require neutral controls AFTER takeover,
  // released PC keys/buttons, and a quiet interval before a fresh pad gesture.
  class input_activity
  {
  public:
    static constexpr std::uint32_t quiet_ms = 750;
    void interrupt(std::uint32_t now) noexcept
    { interrupted_ = true; neutral_ = false; last_pc_ = now; }
    void pc_key(int key, bool down, std::uint32_t now) noexcept
    {
      bool was_down = false;
      if (key >= 0 && key < static_cast<int>(pc_keys_.size()))
      { was_down = pc_keys_[key]; pc_keys_[key] = down; }
      if (down || was_down) interrupt(now);
    }
    bool controller_press(std::uint32_t now) noexcept { return reclaim(now); }
    bool allow_repeat() const noexcept { return !interrupted_; }
    bool analog(const std::array<float, 6>&, bool active, std::uint32_t now) noexcept
    {
      if (!active) { neutral_ = true; return false; }
      const bool allowed = reclaim(now);
      // A gesture begun during quarantine must return to neutral again, rather
      // than turning into fresh input when the timeout eventually expires.
      if (!allowed) neutral_ = false;
      return allowed;
    }
    void reset() noexcept { neutral_ = false; } // Preserve PC quarantine across hotplug.

  private:
    bool interrupted_ = false;
    bool neutral_ = false;
    std::uint32_t last_pc_ = 0;
    std::array<bool, 256> pc_keys_{};
    bool reclaim(std::uint32_t now) noexcept
    {
      if (!interrupted_) return true;
      if (!neutral_ || static_cast<std::uint32_t>(now - last_pc_) < quiet_ms) return false;
      for (bool held : pc_keys_) if (held) return false;
      interrupted_ = false;
      return true;
    }
  };
}
