#include <std_include.hpp>

#include <component/gamepad/controller/engine/bind.hpp>
#include <component/gamepad/controller/mapping/binding.hpp>
#include <component/gamepad/controller/mapping/key.hpp>

namespace gamepad::unstable::controller::engine
{
  namespace { constexpr char custom_layout[] {"custom"}; }

  const char* controller_command_for (const char* command) noexcept
  {
    if (command == nullptr) return nullptr;
    if (std::strcmp (command, "+activate") == 0 || std::strcmp (command, "+reload") == 0)
      return "+usereload";
    if (std::strcmp (command, "+melee_breath") == 0) return "+holdbreath";
    return command;
  }

  bind_bridge::bind_bridge (const context& ctx, const dvars& d) : ctx_ (ctx), dvars_ (d) {}

  void bind_bridge::apply_layout (string_view name)
  {
    mapping::binding_table table;
    mapping::apply_button_layout (table, name);
    size_t bound = 0;
    table.for_each ([&bound] (mapping::engine_key key, const string& command)
    {
      const string text = "bind " + string (mapping::key_name (key)) + " \"" + command + "\"\n";
      game::Cbuf_AddText (0, text.c_str ());
      ++bound;
    });
    ctx_.report (severity::info, facility::mapping, errc::none,
                 "queued controller layout '" + string (name) + "' with " +
                 std::to_string (bound) + " bindings");
  }

  void bind_bridge::apply_configured_layout ()
  {
    const char* name = read (dvars_.buttons_config, "gamepad_default");
    if (std::strcmp (name, custom_layout) != 0) apply_layout (name);
  }

  void bind_bridge::migrate_controller_commands () {}
  void bind_bridge::reapply_layout () { apply_layout (read (dvars_.buttons_config, "gamepad_default")); }

  void bind_bridge::note_manual_rebind () noexcept
  {
    Dvar_SetString (dvars_.buttons_config, custom_layout);
  }

  size_t bind_bridge::command_keys (int client, bool controller_in_use,
                                  const char* command, int (&keys_out)[2]) noexcept
  {
    keys_out[0] = keys_out[1] = -1;
    if (client != 0 || command == nullptr || *command == '\0') return 0;
    // QoS PC 1.1 Key_GetCommandAssignment (10318B10): 256 records, 12-byte
    // stride; binding pointer at 11263624. Called inside CL_GetKeyBinding's
    // native critical section. Do not cache presets: manual binds are authoritative.
    size_t count = 0;
    for (int key = 0; key < 256; ++key)
    {
      if (mapping::is_controller_key (key) != controller_in_use) continue;
      const auto* bound = *reinterpret_cast<const char* const*> (
        game::game_offset (0x11263624 + key * 12));
      if (bound == nullptr || _stricmp (bound, command) != 0) continue;
      keys_out[count++] = key;
      if (count == 2) break;
    }
    return count;
  }
}
