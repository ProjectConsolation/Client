#include <std_include.hpp>
#include <loader/component_loader.hpp>
#include <component/utils/scheduler.hpp>
#include <game/game.hpp>
#include <game/dvars.hpp>
#include <utils/hook.hpp>
#include <component/engine/console/console.hpp>
#include "legacy_switching_policy.hpp"

namespace legacy_switching
{
  namespace
  {
    game::dvar_s* enabled{};
    utils::hook::detour begin_hook, check_hook, raise_hook;
    void* begin_original{};
    void* check_original{};
    void* raise_original{};

    // QoS 1021E320 is called only after FinishWeaponChange has validated and
    // committed the new weapon. EAX=ps, EDX=anim, XMM0=spread, stack=raise time
    // and three alternate-switch flags. Retain its spread/pm-flag/timing side
    // effects, but keep state 1 and do not call StartWeaponAnim while sprinting.
    __declspec(naked) void raise_stub()
    {
      __asm {
        push ecx
        mov ecx, enabled
        test ecx, ecx
        jz stock_raise
        cmp byte ptr [ecx + 16], 0
        je stock_raise
        test dword ptr [eax + 12], 8000h
        jz stock_raise
        mov dword ptr [eax + 244], 1
        movss dword ptr [eax + 4340], xmm0
        test dword ptr [eax + 12], 1
        jz raise_time
        or dword ptr [eax + 12], 200h
      raise_time:
        mov ecx, [esp + 8]
        mov [eax + 64], ecx
        pop ecx
        ret
      stock_raise:
        pop ecx
        jmp dword ptr [raise_original]
      }
    }

    // Behavior adapted from ujicos/iw6x-toybox pan.cpp's sv_mw2mechanics.
    // QoS PC 1.1: PM_BeginWeaponChange=1021E3A0, ps in ESI, two stack
    // arguments; PM_Weapon_CheckForChangeWeapon=1021F1B0, pm in EAX.
    // KisakCOD bg_weapons.cpp corroborates the native drop-cancellation path.
    // QoS owns one weapon state, NOT IW6's two hands. Its state 5 is FIRING,
    // so importing IW6's [3,5] cancellation range would break firing.
    unsigned& field(unsigned char* ps, const unsigned offset)
    {
      return *reinterpret_cast<unsigned*>(ps + offset);
    }

    int __cdecl begin(unsigned char* ps, int weapon, int quick)
    {
      const bool preserve = policy::sprint_animation(enabled && enabled->current.enabled, field(ps, 12));
      const auto animation = field(ps, 4336);
      int result;
      __asm {
        push esi
        mov esi, ps
        push quick
        push weapon
        call dword ptr [begin_original]
        add esp, 8
        pop esi
        mov result, eax
      }
      if (preserve) field(ps, 4336) = animation;
      return result;
    }

    int __cdecl check(unsigned char* pm)
    {
      auto* ps = *reinterpret_cast<unsigned char**>(pm);
      const auto state = field(ps, 244);
      const bool preserve = policy::sprint_animation(enabled && enabled->current.enabled, field(ps, 12));
      const auto weapon = field(ps, 240);
      const auto requested = *reinterpret_cast<const unsigned short*>(pm + 12);
      const auto animation = field(ps, 4336);
      int result;
      __asm {
        mov eax, pm
        call dword ptr [check_original]
        mov result, eax
      }
      // Restore only if the native path actually accepted cancellation. Do not
      // bypass mantle, cover, ladder, ownership, reload or frozen-player checks.
      if (preserve && policy::cancelled_drop(state, weapon, requested, field(ps, 244)))
        field(ps, 4336) = animation;
      return result;
    }

    __declspec(naked) void begin_stub()
    {
      __asm {
        push [esp + 8]
        push [esp + 8]
        push esi
        call begin
        add esp, 12
        ret
      }
    }

    __declspec(naked) void check_stub()
    {
      __asm {
        push eax
        call check
        add esp, 4
        ret
      }
    }
  }

  class component final : public component_interface
  {
  public:
    void post_load() override
    {
      // Fail closed on an unsupported binary rather than patching a function
      // selected by an unreliable cross-title symbol name.
      const unsigned char begin_bytes[] = {0x83, 0xEC, 0x10, 0x53, 0x55};
      const unsigned char check_bytes[] = {0x51, 0x64, 0x8B, 0x0D, 0x2C, 0, 0, 0};
      const unsigned char raise_bytes[] = {0x51, 0x83, 0x7C, 0x24, 0x0C, 0};
      if (std::memcmp(reinterpret_cast<void*>(game::game_offset(0x1021E3A0)), begin_bytes, sizeof(begin_bytes))
          || std::memcmp(reinterpret_cast<void*>(game::game_offset(0x1021F1B0)), check_bytes, sizeof(check_bytes))
          || std::memcmp(reinterpret_cast<void*>(game::game_offset(0x1021E320)), raise_bytes, sizeof(raise_bytes)))
      {
        console::warn("[movement] legacy switching skipped: unexpected QoS weapon ABI\n");
        return;
      }
      begin_hook.create(game::game_offset(0x1021E3A0), begin_stub);
      check_hook.create(game::game_offset(0x1021F1B0), check_stub);
      begin_original = begin_hook.get_original();
      check_original = check_hook.get_original();
      raise_hook.create(game::game_offset(0x1021E320), raise_stub);
      raise_original = raise_hook.get_original();
      scheduler::once([] {
        enabled = dvars::Dvar_RegisterBool("pm_legacySwitching", false,
          "Suppress sprint switch animations; retain native switching validation, state 1 and raise timing (0 = stock QoS).",
          game::dvar_flags::replicated);
      }, scheduler::main);
    }

    void pre_destroy() override
    {
      raise_hook.clear();
      check_hook.clear();
      begin_hook.clear();
    }
  };
}

REGISTER_COMPONENT(legacy_switching::component)
