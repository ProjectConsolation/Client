#pragma once
#include <array>

namespace gamepad::unstable::controller::engine
{
  // A held trigger/stick or generated repeat is not fresh input after PC takeover.
  // Compare against the interruption sample, not frame-to-frame noise, so slow
  // deliberate movement accumulates and can still reclaim controller ownership.
  class input_activity
  {
  public:
    void interrupt() noexcept { interrupted_ = true; baseline_ = latest_; }
    void controller_press() noexcept { interrupted_ = false; }
    bool allow_repeat() const noexcept { return !interrupted_; }
    bool analog(const std::array<float, 6>& value, bool active) noexcept
    {
      latest_ = value;
      if (!active) return false;
      if (!interrupted_) return true;
      for (std::size_t i = 0; i < value.size(); ++i)
      {
        const float difference = value[i] - baseline_[i];
        if (difference >= 0.05f || difference <= -0.05f)
        {
          interrupted_ = false;
          return true;
        }
      }
      return false;
    }
    void reset() noexcept { *this = input_activity{}; }

  private:
    bool interrupted_ = false;
    std::array<float, 6> latest_{}, baseline_{};
  };
}
