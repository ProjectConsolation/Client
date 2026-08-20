#include <std_include.hpp>

#include <component/gamepad/controller/engine/view.hpp>

namespace gamepad::unstable::controller::engine
{
  using namespace aim;

  namespace
  {
    constexpr float move_scale {127.0f};

    signed char clamp_move (int value) noexcept
    {
      return static_cast<signed char> (std::clamp (value, -128, 127));
    }

    float sensitivity_scale (const dvars& d) noexcept
    {
      const float view = std::clamp (read (d.view_sensitivity, 1.0f), 0.01f, 30.0f);
      const float level = static_cast<float> (
        std::clamp (read (d.sensitivity_level, 5), 1, 10)) / 5.0f;
      return view * level;
    }

    aim_profile make_profile (const dvars& d, float yaw, float pitch)
    {
      aim_profile profile;
      profile.yaw_rate = deg_per_s {yaw};
      profile.pitch_rate = deg_per_s {pitch};
      profile.deadzone = deadzone_params {
        magnitude {read (d.stick_deadzone_min, 0.2f)},
        magnitude {read (d.stick_deadzone_max, 0.01f)}, magnitude {0.0f}};
      profile.curve = response_curve {curve_kind::linear, 1.0f};
      return profile;
    }

    size_t tuning_signature (const dvars& d)
    {
      const float values[] {
        read (d.turnrate_yaw, 260.0f), read (d.turnrate_yaw_ads, 90.0f),
        read (d.turnrate_pitch, 90.0f), read (d.turnrate_pitch_ads, 55.0f),
        read (d.stick_deadzone_min, 0.2f), read (d.stick_deadzone_max, 0.01f),
        read (d.accel_rate, 1200.0f), static_cast<float> (read (d.accel_enabled, true))
      };
      size_t result = 0;
      for (float value: values) result = result * 31u + std::hash<float> {} (value);
      return result;
    }

    bool in_region (const AimScreenTarget& target, float width, float height) noexcept
    {
      return width >= target.clipMins[0] && target.clipMaxs[0] >= -width &&
             height >= target.clipMins[1] && target.clipMaxs[1] >= -height;
    }

    const AimScreenTarget* best_target (const AimAssistGlobals& globals,
                                        float width, float height,
                                        float range_scale) noexcept
    {
      if (globals.screenTargetCount < 0 || globals.screenTargetCount > 64) return nullptr;
      const float range = 8192.0f * std::max (range_scale, 0.0f);
      const float range_sqr = range * range;
      for (int i = 0; i < globals.screenTargetCount; ++i)
      {
        const auto& target = globals.screenTargets[i];
        if (std::isfinite (target.distSqr) && target.distSqr > 0.0f &&
            target.distSqr <= range_sqr && in_region (target, width, height))
          return &target;
      }
      return nullptr;
    }

    void store_melee_output (usercmd_s& cmd, const AimOutput& output) noexcept
    {
      std::memcpy (reinterpret_cast<std::uint8_t*> (&cmd) + 0x24,
                   &output.meleeChargeYaw, sizeof (output.meleeChargeYaw));
      *(reinterpret_cast<std::uint8_t*> (&cmd) + 0x28) = output.meleeChargeDist;
    }
  }

  view_driver::view_driver (const context& ctx, const dvars& d) : ctx_ (ctx), dvars_ (d) {}

  bool view_driver::ensure_processor ()
  {
    const size_t signature = tuning_signature (dvars_);
    if (have_signature_ && signature == tuning_signature_ && processor_) return true;
    tuning_signature_ = signature;
    have_signature_ = true;

    aim_settings settings;
    settings.hip = make_profile (dvars_, read (dvars_.turnrate_yaw, 260.0f),
                                 read (dvars_.turnrate_pitch, 90.0f));
    settings.ads = make_profile (dvars_, read (dvars_.turnrate_yaw_ads, 90.0f),
                                 read (dvars_.turnrate_pitch_ads, 55.0f));
    const float acceleration = read (dvars_.accel_enabled, true)
      ? read (dvars_.accel_rate, 1200.0f) : 0.0f;
    settings.accel = turn_integrator::limits {deg_per_s2 {acceleration}, deg_per_s2 {0.0f}};
    // QoS stores dvar pointers at 0x1149E108, not an inline GraphFloat array.
    // Keep the safe linear profile until the real graph storage is identified.
    settings.graph_knots.reset ();
    settings.graph_monotonic = false;

    string reason;
    auto calibration = aim_calibration::make (settings, reason);
    if (!calibration)
    {
      processor_.reset ();
      calibration_.reset ();
      if (!reported_invalid_)
      {
        reported_invalid_ = true;
        ctx_.report (severity::warning, facility::aim, errc::graph_invalid,
                     "aim configuration rejected (" + reason + ")");
      }
      return false;
    }

    reported_invalid_ = false;
    calibration_.emplace (move (*calibration));
    processor_.emplace (calibration_->processor_config ());
    return true;
  }

  void view_driver::observe (const canonical_sample& sample) noexcept
  {
    const auto left = sample.sticks[static_cast<size_t> (stick::left)].calibrated;
    const auto right = sample.sticks[static_cast<size_t> (stick::right)].calibrated;
    const mapping::stick_layout layout = mapping::stick_layout_from_name (
      read (dvars_.sticks_config, "thumbstick_default"));
    axes_ = mapping::resolve (layout, left, right);
    if (!std::isfinite (axes_.side) || !std::isfinite (axes_.forward) ||
        !std::isfinite (axes_.yaw) || !std::isfinite (axes_.pitch))
      axes_ = {};
  }

  void view_driver::idle () noexcept
  {
    axes_ = {};
    if (processor_) processor_->reset ();
  }

  bool view_driver::view_active (int) const noexcept { return !menu_or_console_active (); }

  void view_driver::apply_move (int client, usercmd_s& cmd, float frame_time) noexcept
  {
    cmd.forwardmove = clamp_move (cmd.forwardmove + static_cast<int> (std::lround (axes_.forward * move_scale)));
    cmd.rightmove = clamp_move (cmd.rightmove + static_cast<int> (std::lround (axes_.side * move_scale)));
    if (!view_active (client) || !ensure_processor ()) return;

    frame_time = std::clamp (frame_time, 0.0f, 0.05f);

    AimInput input {};
    AimOutput output {};
    input.deltaTime = frame_time;
    input.pitch = view_pitch ();
    input.pitchAxis = axes_.pitch;
    input.yaw = view_yaw ();
    input.yawAxis = axes_.yaw;
    input.forwardAxis = axes_.forward;
    input.rightAxis = axes_.side;
    input.buttons = static_cast<int> (cmd.buttons);
    input.localClientNum = client;
    input.playerState = reinterpret_cast<void*> (game::game_offset (0x12A4CDFC));

    // QoS refreshes targets and applies its surviving auto-aim/auto-melee here.
    AimAssist_UpdateGamePadInput (&input, &output);
    if (std::isfinite (output.pitch))
      view_pitch () = std::clamp (output.pitch, -85.0f, 85.0f);
    if (std::isfinite (output.yaw))
      view_yaw () = output.yaw;
    store_melee_output (cmd, output);

    stick_vector look {axes_.yaw, axes_.pitch};
    if (read (dvars_.scale_view_axis, true)) look = scale_dominant_axis (look);

    const AimAssistGlobals& globals = aim_globals (client);
    const bool assist_enabled = read (dvars_.aim_assist_enabled, true);
    const float assist_range_scale = read (dvars_.aim_assist_range_scale, 1.0f);
    aim_frame_input frame;
    frame.look = look;
    frame.ads_lerp = globals.initialized ? std::clamp (globals.adsLerp, 0.0f, 1.0f) : 0.0f;
    frame.fov_scale = 1.0f;
    frame.sensitivity = sensitivity_scale (dvars_);
    frame.invert_pitch = read (dvars_.invert_pitch, false);
    frame.dt = seconds {frame_time};

    if (assist_enabled && globals.initialized && read (dvars_.slowdown_enabled, true) &&
        best_target (globals, globals.slowdownRegionWidth, globals.slowdownRegionHeight,
                     assist_range_scale))
    {
      frame.slowdown_yaw = slowdown_scale (true, 0.4f, 0.5f, globals.adsLerp);
      frame.slowdown_pitch = slowdown_scale (true, 0.4f, 0.5f, globals.adsLerp);
    }

    aim_frame_output result = processor_->process (frame);
    if (assist_enabled && globals.initialized && read (dvars_.lockon_enabled, false) &&
        std::hypot (look.x, look.y) <= read (dvars_.lockon_deflection, 0.05f))
    {
      if (const auto* target = best_target (globals, globals.lockOnRegionWidth,
                                            globals.lockOnRegionHeight,
                                            assist_range_scale))
      {
        lock_on_target lock_target;
        lock_target.target_velocity = {target->velocity[0], target->velocity[1], target->velocity[2]};
        lock_target.player_velocity = {globals.playerVelocity[0], globals.playerVelocity[1], globals.playerVelocity[2]};
        lock_target.view_yaw_axis = {-globals.viewAxis[1][0], -globals.viewAxis[1][1], -globals.viewAxis[1][2]};
        lock_target.view_pitch_axis = {globals.viewAxis[2][0], globals.viewAxis[2][1], globals.viewAxis[2][2]};
        lock_target.distance = std::sqrt (target->distSqr);
        lock_on_params params;
        params.yaw_strength = read (dvars_.lockon_strength, 0.6f);
        params.pitch_strength = read (dvars_.lockon_pitch_strength, 0.0f);
        const auto correction = lock_on (lock_target, params, seconds {frame_time});
        if (std::isfinite (correction.yaw_delta.value) &&
            std::isfinite (correction.pitch_delta.value))
        {
          result.yaw_delta = result.yaw_delta + correction.yaw_delta;
          result.pitch_delta = result.pitch_delta + correction.pitch_delta;
        }
      }
    }

    if (std::isfinite (result.yaw_delta.value) &&
        std::isfinite (result.pitch_delta.value))
    {
      view_yaw () -= result.yaw_delta.value;
      view_pitch () = std::clamp (view_pitch () - result.pitch_delta.value, -85.0f, 85.0f);
    }
  }

  void view_driver::apply_remote_move (int, usercmd_s&) noexcept {}
}
