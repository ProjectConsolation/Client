#pragma once

#include "game/game.hpp"
#include "game/dvars.hpp"

namespace gamepad::unstable::controller::engine
{
  using dvar_t = game::dvar_s;
  using usercmd_s = game::usercmd_t;

  inline constexpr int DVAR_NONE = game::dvar_flags::none;
  inline constexpr int DVAR_ARCHIVE = game::dvar_flags::saved;
  inline constexpr int DVAR_ROM = game::dvar_flags::read_only;
  inline constexpr int DVAR_CHEAT = game::dvar_flags::cheat_protected;

  inline constexpr int K_ENTER = game::K_ENTER;
  inline constexpr int K_ESCAPE = game::K_ESCAPE;
  inline constexpr int K_UPARROW = game::K_UPARROW;
  inline constexpr int K_DOWNARROW = game::K_DOWNARROW;
  inline constexpr int K_LEFTARROW = game::K_LEFTARROW;
  inline constexpr int K_RIGHTARROW = game::K_RIGHTARROW;

  dvar_t* Dvar_FindVar (const char* name) noexcept;
  dvar_t* Dvar_RegisterBool (const char*, bool, int, const char*);
  dvar_t* Dvar_RegisterFloat (const char*, float, float, float, int, const char*);
  dvar_t* Dvar_RegisterInt (const char*, int, int, int, int, const char*);
  dvar_t* Dvar_RegisterString (const char*, const char*, int, const char*);
  void Dvar_SetBool (dvar_t*, bool) noexcept;
  void Dvar_SetInt (dvar_t*, int) noexcept;
  void Dvar_SetString (dvar_t*, const char*) noexcept;

  int Sys_Milliseconds () noexcept;
  bool menu_or_console_active () noexcept;
  void emit_key (int key, bool down, unsigned time) noexcept;

#pragma pack(push, 1)
  struct AimScreenTarget
  {
    int entityNum;
    float clipMins[2];
    float clipMaxs[2];
    float position[3];
    float velocity[3];
    float distSqr;
    float crosshairDistSqr;
  };

  struct AimAssistGlobals
  {
    std::uint8_t initialized;
    std::uint8_t pad0001[3];
    float slowdownRegionWidth;
    float slowdownRegionHeight;
    std::uint8_t pad000C[0x10];
    float lockOnRegionWidth;
    float lockOnRegionHeight;
    std::uint8_t pad0024[0x0C];
    float playerVelocity[3];
    float viewAxis[3][3];
    std::uint8_t pad0060[8];
    float adsLerp;
    std::uint8_t pad006C[0x90];
    AimScreenTarget screenTargets[64];
    int screenTargetCount;
    std::uint8_t pad0E00[0x34];
  };
#pragma pack(pop)

  static_assert (offsetof (AimAssistGlobals, screenTargets) == 0xFC);
  static_assert (offsetof (AimAssistGlobals, screenTargetCount) == 0xDFC);
  static_assert (sizeof (AimAssistGlobals) == 0xE34);

  AimAssistGlobals& aim_globals (int client) noexcept;
  float& view_pitch () noexcept;
  float& view_yaw () noexcept;
  float frame_seconds () noexcept;
}
