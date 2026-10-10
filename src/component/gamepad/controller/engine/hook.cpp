#include <std_include.hpp>

#include <component/gamepad/controller/engine/hook.hpp>
#include <component/gamepad/controller/engine/import.hpp>
#include <component/gamepad/controller/runtime.hpp>
#include <component/gamepad/controller/mapping/key.hpp>
#include <component/gamepad/controller/engine/icons.hpp>
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
    utils::hook::detour command_assignment_hook, display_key_name_hook;
    void* display_key_name_original {};

    int __cdecl command_assignment_body(int client, const char* command, int* output) noexcept
    {
      int found[2];
      const auto count = bind_bridge::command_keys(client,
        active_runtime != nullptr && active_runtime->driving(), command, found);
      output[0] = found[0];
      output[1] = found[1];
      return static_cast<int>(count);
    }

    // QoS 10318B10: EAX client, two caller-clean stack arguments. Retain
    // CL_GetKeyBinding's native lock and CG_FindBindedDirective's alias search.
    __declspec(naked) void command_assignment_stub()
    {
      __asm {
        push [esp + 8]
        push [esp + 8]
        push eax
        call command_assignment_body
        add esp, 12
        ret
      }
    }

    char* __cdecl display_key_name_body(int key, char* output) noexcept
    {
      const auto mapped = mapping::key_from_keynum(key);
      if (!mapped) return nullptr;
      const char* label = active_runtime && active_runtime->driving()
        ? displayed_button_icon(*active_runtime, *mapped) : nullptr;
      // Missing/unsupported artwork must remain a readable button label, not
      // a bogus icon pointer, a key-zero substitution, or an unbound action.
      if (!label) label = mapping::key_name(*mapped);
      strncpy_s(output, 128, label, _TRUNCATE);
      return output;
    }

    // QoS 102FFFE0: ECX key, ESI output[128], EAX result. This is the
    // localized DISPLAY path, not the config key-name serialization path.
    __declspec(naked) void display_key_name_stub()
    {
      __asm {
        push ecx
        push esi
        push ecx
        call display_key_name_body
        add esp, 8
        test eax, eax
        jz stock_key
        pop ecx
        ret
      stock_key:
        pop ecx
        jmp dword ptr [display_key_name_original]
      }
    }

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
      // QoS's CL_MouseMove is not just mouse look. It also advances native client
      // and aim-assist state that must run during map transitions and gameplay.
      // Preserve that engine work, then layer controller input onto its usercmd.
      const auto function = static_cast<int> (game::game_offset (0x102FC4D0));
      __asm {
        push cmd
        mov eax, client
        call function
        add esp, 4
      }

      if (active_runtime != nullptr && active_runtime->driving () &&
          !menu_or_console_active ())
      {
        active_runtime->view ().apply_move (client, *cmd, frame_seconds ());
        patches::enforce_ads_sprint_interrupt (cmd);
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
    install_prompt_icons();
    command_assignment_hook.create(game::game_offset(0x10318B10), command_assignment_stub);
    display_key_name_hook.create(game::game_offset(0x102FFFE0), display_key_name_stub);
    display_key_name_original = display_key_name_hook.get_original();
    keynum_to_string_hook.create (game::game_offset (0x103185A0), keynum_to_string_stub);
    keynum_to_string_original = keynum_to_string_hook.get_original ();
    string_to_keynum_hook.create (game::game_offset (0x10318B80), string_to_keynum_stub);
    string_to_keynum_original = string_to_keynum_hook.get_original ();
    utils::hook::call (game::game_offset (0x102FFBFE), mouse_move_stub);
    rt.make_context ().report (severity::info, facility::engine, errc::none,
                               "QoS controller input hooks installed; native usercmd codec retained");
  }
}
