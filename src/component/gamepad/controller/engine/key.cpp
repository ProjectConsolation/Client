#include <std_include.hpp>

#include <component/gamepad/controller/engine/key.hpp>
#include <component/gamepad/controller/aim/deadzone.hpp>
#include <component/gamepad/controller/engine/import.hpp>
#include <component/gamepad/controller/mapping/physical.hpp>

namespace gamepad::unstable::controller::engine
{
  using mapping::engine_key;

  namespace
  {
    constexpr int scroll_delay_first {420};
    constexpr int scroll_delay_rest {210};
    constexpr int scroll_delay_min {50};
    constexpr int scroll_accel_time {1500};
    constexpr int controller_use_hold_time {250};

    struct button_key { button physical; engine_key key; };
    constexpr button_key button_keys[] {
      {button::face_west, engine_key::button_x}, {button::face_south, engine_key::button_a},
      {button::face_east, engine_key::button_b}, {button::face_north, engine_key::button_y},
      {button::l2, engine_key::button_ltrig}, {button::r2, engine_key::button_rtrig},
      {button::l1, engine_key::button_lshldr}, {button::r1, engine_key::button_rshldr},
      {button::start, engine_key::button_start}, {button::back, engine_key::button_back},
      {button::l3, engine_key::button_lstick}, {button::r3, engine_key::button_rstick},
      {button::dpad_up, engine_key::dpad_up}, {button::dpad_down, engine_key::dpad_down},
      {button::dpad_left, engine_key::dpad_left}, {button::dpad_right, engine_key::dpad_right},
    };

    bool repeats_while_held (button b) noexcept
    {
      return b == button::dpad_up || b == button::dpad_down ||
             b == button::dpad_left || b == button::dpad_right ||
             b == button::l2 || b == button::r2;
    }

    bool is_dpad (engine_key k) noexcept
    {
      return k == engine_key::dpad_up || k == engine_key::dpad_down ||
             k == engine_key::dpad_left || k == engine_key::dpad_right;
    }

    bool is_stick_key (engine_key k) noexcept
    {
      return k == engine_key::apad_up || k == engine_key::apad_down ||
             k == engine_key::apad_left || k == engine_key::apad_right ||
             k == engine_key::rstick_up || k == engine_key::rstick_down ||
             k == engine_key::rstick_left || k == engine_key::rstick_right;
    }

    bool is_scroll_key (engine_key k) noexcept { return is_dpad (k) || is_stick_key (k); }

    int menu_key (engine_key k) noexcept
    {
      switch (k)
      {
      case engine_key::button_a:
      case engine_key::button_start: return K_ENTER;
      case engine_key::button_b:
      case engine_key::button_back: return K_ESCAPE;
      case engine_key::dpad_up:
      case engine_key::apad_up:
      case engine_key::rstick_up: return K_UPARROW;
      case engine_key::dpad_down:
      case engine_key::apad_down:
      case engine_key::rstick_down: return K_DOWNARROW;
      case engine_key::dpad_left:
      case engine_key::apad_left:
      case engine_key::rstick_left: return K_LEFTARROW;
      case engine_key::dpad_right:
      case engine_key::apad_right:
      case engine_key::rstick_right: return K_RIGHTARROW;
      default: return 0;
      }
    }

    engine_key stick_key (stick which, bool horizontal, bool positive) noexcept
    {
      const auto direction = horizontal
        ? (positive ? mapping::stick_direction::right : mapping::stick_direction::left)
        : (positive ? mapping::stick_direction::up : mapping::stick_direction::down);
      return mapping::to_engine_key (mapping::apad_input {which, direction});
    }

    const char* fallback_command (engine_key key) noexcept
    {
      switch (key)
      {
      case engine_key::button_a: return "+gostand";
      case engine_key::button_b: return "+stance";
      case engine_key::button_x: return "+usereload";
      case engine_key::button_y: return "weapnext";
      case engine_key::button_ltrig: return "+speed_throw";
      case engine_key::button_rtrig: return "+attack";
      case engine_key::button_lshldr: return "+smoke";
      case engine_key::button_rshldr: return "+frag";
      case engine_key::button_start: return "togglemenu";
      case engine_key::button_back: return "+scores";
      case engine_key::button_lstick: return "+breath_sprint";
      case engine_key::button_rstick: return "+melee";
      case engine_key::dpad_up: return "+actionslot 1";
      case engine_key::dpad_down: return "+actionslot 2";
      case engine_key::dpad_left: return "+actionslot 3";
      case engine_key::dpad_right: return "+actionslot 4";
      default: return nullptr;
      }
    }

    const char* engine_binding (engine_key key) noexcept
    {
      const int keynum = static_cast<int> (key);
      if (keynum < 0 || keynum > 255) return nullptr;
      return *reinterpret_cast<const char* const*> (
        game::game_offset (0x11263624 + keynum * 12));
    }

    void queue_fallback (engine_key key, bool down, unsigned time)
    {
      const char* command = fallback_command (key);
      if (command == nullptr || *command == '\0') return;

      char text[256] {};
      if (*command == '+')
        sprintf_s (text, down ? "%s %d %u\n" : "-%s %d %u\n",
                   down ? command : command + 1, static_cast<int> (key), time);
      else if (down)
        sprintf_s (text, "%s\n", command);
      else
        return;

      game::Cbuf_AddText (0, text);
    }
  }

  key_dispatcher::key_dispatcher (const context& ctx, const dvars& d) : ctx_ (ctx), dvars_ (d) {}

  void key_dispatcher::set_in_use (bool value) noexcept
  {
    if (in_use_ == value) return;
    in_use_ = value;
    Dvar_SetBool (dvars_.in_use, value);
    apply_use_hold_time ();
  }

  void key_dispatcher::apply_use_hold_time () noexcept
  {
    if (use_hold_time_ == nullptr) use_hold_time_ = Dvar_FindVar ("g_useholdtime");
    const int desired = in_use_ ? controller_use_hold_time : 0;
    if (use_hold_time_ != nullptr && read (use_hold_time_, 0) != desired)
      Dvar_SetInt (use_hold_time_, desired);
  }

  void key_dispatcher::note_other_input () noexcept { set_in_use (false); }

  void key_dispatcher::dispatch (const canonical_sample& sample) noexcept
  {
    const unsigned time = static_cast<unsigned> (Sys_Milliseconds ());
    aim::deadzone_params dz {aim::magnitude {read (dvars_.stick_deadzone_min, 0.2f)},
                             aim::magnitude {read (dvars_.stick_deadzone_max, 0.01f)},
                             aim::magnitude {0.0f}};
    string why;
    if (!aim::validate (dz, why))
    {
      dz = {aim::magnitude {0.2f}, aim::magnitude {0.01f}, aim::magnitude {0.0f}};
      if (!reported_deadzone_)
      {
        reported_deadzone_ = true;
        ctx_.report (severity::warning, facility::mapping, errc::calibration_invalid,
                     "invalid stick deadzone; using defaults");
      }
    }

    const auto left = aim::apply (dz, sample.sticks[static_cast<size_t> (stick::left)].calibrated);
    const auto right = aim::apply (dz, sample.sticks[static_cast<size_t> (stick::right)].calibrated);
    const float axis[axis_count] {right.x, right.y, left.x, left.y};
    const float pressed = read (dvars_.stick_pressed, 0.4f);
    const float hysteresis = read (dvars_.stick_pressed_hysteresis, 0.1f);

    for (size_t i = 0; i < axis_count; ++i)
      for (size_t end = 0; end < 2; ++end)
      {
        was_deflected_[i][end] = deflected_[i][end];
        deflected_[i][end] = mapping::axis_deflected (
          axis[i], end == 1, was_deflected_[i][end], pressed, hysteresis);
      }

    const float lt = sample.triggers[static_cast<size_t> (trigger_side::left)].normalized;
    const float rt = sample.triggers[static_cast<size_t> (trigger_side::right)].normalized;
    if (left.x != 0 || left.y != 0 || right.x != 0 || right.y != 0 ||
        lt >= read (dvars_.button_deadzone, 0.13f) || rt >= read (dvars_.button_deadzone, 0.13f))
      set_in_use (true);

    dispatch_apad (time);
    dispatch_buttons (sample, time);
    buttons_ = sample.buttons;
    apply_use_hold_time ();
  }

  void key_dispatcher::dispatch_apad (unsigned time) noexcept
  {
    const bool menu = menu_or_console_active ();
    for (size_t i = 0; i < axis_count; ++i)
    {
      const stick which = i < 2 ? stick::right : stick::left;
      const bool horizontal = (i % 2) == 0;
      const engine_key positive = stick_key (which, horizontal, true);
      const engine_key negative = stick_key (which, horizontal, false);

      if (!menu)
      {
        if (was_deflected_[i][1]) emit (positive, key_event::released, time);
        if (was_deflected_[i][0]) emit (negative, key_event::released, time);
        continue;
      }

      if (deflected_[i][1]) emit (positive, was_deflected_[i][1] ? key_event::repeated : key_event::pressed, time);
      else if (deflected_[i][0]) emit (negative, was_deflected_[i][0] ? key_event::repeated : key_event::pressed, time);
      else if (was_deflected_[i][1]) emit (positive, key_event::released, time);
      else if (was_deflected_[i][0]) emit (negative, key_event::released, time);
    }
  }

  void key_dispatcher::dispatch_buttons (const canonical_sample& sample, unsigned time) noexcept
  {
    for (const auto& item: button_keys)
    {
      const bool now = sample.buttons.down (item.physical);
      const bool was = buttons_.down (item.physical);
      if (now && !was) emit_button (item.key, key_event::pressed, time);
      else if (now && repeats_while_held (item.physical)) emit_button (item.key, key_event::repeated, time);
      else if (!now && was) emit_button (item.key, key_event::released, time);
    }
  }

  void key_dispatcher::emit_button (engine_key key, key_event event, unsigned time) noexcept
  {
    set_in_use (true);
    if (menu_or_console_active () && event != key_event::repeated)
      reset_scroll (key, event == key_event::pressed, time);
    emit (key, event, time);
  }

  void key_dispatcher::emit (engine_key key, key_event event, unsigned time) noexcept
  {
    const bool down = event != key_event::released;
    if (is_stick_key (key) && event == key_event::pressed)
      reset_scroll (key, true, time);
    else if (is_stick_key (key) && event == key_event::released)
      reset_scroll (key, false, time);
    if (event == key_event::repeated && ignore_repeat (key, 2, time)) return;

    if (menu_or_console_active ())
    {
      if (const int translated = menu_key (key); translated != 0)
        emit_key (translated, down, time);
    }
    else if (static_cast<int> (key) < 0xE0)
    {
      emit_key (static_cast<int> (key), down, time);
      const char* binding = engine_binding (key);
      if (binding == nullptr || *binding == '\0')
        queue_fallback (key, down, time);
    }
  }

  bool key_dispatcher::ignore_repeat (engine_key key, int repeats, unsigned time) noexcept
  {
    if (menu_or_console_active () && is_scroll_key (key))
    {
      if (repeats == 1) { next_scroll_ = time + scroll_delay_first; return false; }
      if (time >= next_scroll_)
      {
        int delay = scroll_delay_rest;
        if (is_dpad (key))
        {
          const int elapsed = std::min (static_cast<int> (time - scroll_hold_start_), scroll_accel_time);
          delay -= (scroll_delay_rest - scroll_delay_min) * elapsed / scroll_accel_time;
        }
        next_scroll_ = time + static_cast<unsigned> (delay);
        return false;
      }
    }
    return repeats > 1;
  }

  void key_dispatcher::reset_scroll (engine_key key, bool down, unsigned time) noexcept
  {
    if (!down) { if (scroll_hold_key_ == key) scroll_hold_key_.reset (); return; }
    if (!is_scroll_key (key)) return;
    if (is_dpad (key) && scroll_hold_key_ != key)
    {
      scroll_hold_start_ = time;
      scroll_hold_key_ = key;
    }
    next_scroll_ = time + scroll_delay_first;
  }

  void key_dispatcher::menu_key_event (engine_key key, bool down) noexcept
  {
    if (const int translated = menu_key (key); translated != 0)
      emit_key (translated, down, static_cast<unsigned> (Sys_Milliseconds ()));
  }

  bool key_dispatcher::scoreboard_key_event (engine_key) noexcept { return false; }

  void key_dispatcher::release_all () noexcept
  {
    const unsigned time = static_cast<unsigned> (Sys_Milliseconds ());
    for (engine_key key: mapping::all_engine_keys) emit (key, key_event::released, time);
    buttons_ = button_set {};
    deflected_ = {};
    was_deflected_ = {};
    scroll_hold_key_.reset ();
  }
}
