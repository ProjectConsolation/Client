#include <std_include.hpp>

#include <component/gamepad/controller/engine/import.hpp>
#include <component/engine/console/game_console.hpp>

namespace gamepad::unstable::controller::engine
{
  namespace
  {
    template <typename T>
    void set_value (dvar_t* dvar, T value) noexcept
    {
      if (dvar == nullptr)
        return;

      if constexpr (std::is_same_v<T, bool>)
        dvar->current.enabled = dvar->latched.enabled = dvar->reset.enabled = value;
      else
        dvar->current.integer = dvar->latched.integer = dvar->reset.integer = value;
    }
  }

  dvar_t* Dvar_FindVar (const char* name) noexcept { return game::Dvar_FindVar (name); }

  dvar_t* Dvar_RegisterBool (const char* name, bool value, int flags, const char* desc)
  {
    if (auto* existing = game::Dvar_FindVar (name)) return existing;
    return dvars::Dvar_RegisterBool (name, value ? 1 : 0, desc, static_cast<std::uint16_t> (flags));
  }

  dvar_t* Dvar_RegisterFloat (const char* name, float value, float min, float max,
                              int flags, const char* desc)
  {
    if (auto* existing = game::Dvar_FindVar (name)) return existing;
    return dvars::Dvar_RegisterFloat (name, desc, value, min, max, static_cast<std::uint16_t> (flags));
  }

  dvar_t* Dvar_RegisterInt (const char* name, int value, int min, int max,
                            int flags, const char* desc)
  {
    if (auto* existing = game::Dvar_FindVar (name)) return existing;
    return dvars::Dvar_RegisterInt (name, desc, value, min, max, static_cast<std::uint16_t> (flags));
  }

  dvar_t* Dvar_RegisterString (const char* name, const char* value, int flags, const char* desc)
  {
    if (auto* existing = game::Dvar_FindVar (name)) return existing;
    return dvars::Dvar_RegisterString (name, value, desc, static_cast<std::uint16_t> (flags));
  }

  void Dvar_SetBool (dvar_t* dvar, bool value) noexcept { set_value (dvar, value); }
  void Dvar_SetInt (dvar_t* dvar, int value) noexcept { set_value (dvar, value); }
  void Dvar_SetString (dvar_t* dvar, const char* value) noexcept
  {
    if (dvar != nullptr) game::Dvar_SetString (dvar->name, value);
  }

  int Sys_Milliseconds () noexcept { return static_cast<int> (GetTickCount ()); }

  bool menu_or_console_active () noexcept
  {
    const auto* ingame = game::Dvar_FindVar ("cl_ingame");
    return game_console::is_active () || ingame == nullptr || !ingame->current.enabled ||
           (game::keyCatchers != nullptr && *game::keyCatchers != 0);
  }

  void emit_key (int key, bool down, unsigned time) noexcept
  {
    game::CL_KeyEvent (0, key, down ? 1 : 0, time);
  }

  AimAssistGlobals& aim_globals (int client) noexcept
  {
    return *reinterpret_cast<AimAssistGlobals*> (
      game::game_offset (0x1149D290 + client * sizeof (AimAssistGlobals)));
  }

  float& view_pitch () noexcept
  {
    return *reinterpret_cast<float*> (game::game_offset (0x11A9FEC0));
  }

  float& view_yaw () noexcept
  {
    return *reinterpret_cast<float*> (game::game_offset (0x11A9FEC4));
  }

  float frame_seconds () noexcept
  {
    const int msec = std::clamp (*reinterpret_cast<int*> (game::game_offset (0x11255D70)), 0, 200);
    return static_cast<float> (msec) * 0.001f;
  }

}
