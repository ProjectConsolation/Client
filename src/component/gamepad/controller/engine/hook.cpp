#include <std_include.hpp>

#include <component/gamepad/controller/engine/hook.hpp>
#include <component/gamepad/controller/engine/import.hpp>
#include <component/gamepad/controller/runtime.hpp>
#include <component/gamepad/controller/mapping/key.hpp>
#include <component/engine/patches/patches.hpp>
#include <utils/hook.hpp>

namespace gamepad::unstable::controller::engine
{
  namespace
  {
    runtime* active_runtime {};
    utils::hook::detour keynum_to_string_hook;
    utils::hook::detour string_to_keynum_hook;
    void* keynum_to_string_original {};
    void* string_to_keynum_original {};

    const char* __cdecl controller_key_name (int key) noexcept
    {
      const auto mapped = mapping::key_from_keynum (key);
      return mapped ? mapping::key_name (*mapped) : nullptr;
    }

    int __cdecl controller_key_from_name (const char* name) noexcept
    {
      if (name == nullptr) return -1;
      const auto mapped = mapping::key_from_name (name);
      return mapped ? static_cast<int> (*mapped) : -1;
    }

    __declspec(naked) void keynum_to_string_stub ()
    {
      __asm {
        push eax
        push eax
        call controller_key_name
        add esp, 4
        test eax, eax
        jnz found
        pop eax
        jmp dword ptr [keynum_to_string_original]
      found:
        add esp, 4
        ret
      }
    }

    __declspec(naked) void string_to_keynum_stub ()
    {
      __asm {
        push edi
        push edi
        call controller_key_from_name
        add esp, 4
        cmp eax, -1
        jne found
        pop edi
        jmp dword ptr [string_to_keynum_original]
      found:
        add esp, 4
        ret
      }
    }

    void __cdecl mouse_move_body (usercmd_s* cmd, int client)
    {
      if (active_runtime != nullptr && active_runtime->driving () &&
          !menu_or_console_active ())
      {
        active_runtime->view ().apply_move (client, *cmd, frame_seconds ());
        patches::enforce_ads_sprint_interrupt (cmd);
        return;
      }

      const auto function = static_cast<int> (game::game_offset (0x102FC4D0));
      __asm {
        push cmd
        mov eax, client
        call function
        add esp, 4
      }
    }

    __declspec(naked) void mouse_move_stub ()
    {
      __asm {
        mov edx, [esp + 4]
        push eax
        push edx
        call mouse_move_body
        add esp, 8
        ret
      }
    }
  }

  void install (runtime& rt)
  {
    active_runtime = &rt;
    keynum_to_string_hook.create (game::game_offset (0x103185A0), keynum_to_string_stub);
    keynum_to_string_original = keynum_to_string_hook.get_original ();
    string_to_keynum_hook.create (game::game_offset (0x10318B80), string_to_keynum_stub);
    string_to_keynum_original = string_to_keynum_hook.get_original ();
    utils::hook::call (game::game_offset (0x102FFBFE), mouse_move_stub);
    rt.make_context ().report (severity::info, facility::engine, errc::none,
                               "QoS controller input hooks installed; native usercmd codec retained");
  }
}
