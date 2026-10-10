#include <std_include.hpp>

#include <component/gamepad/controller/engine/command.hpp>
#include <component/gamepad/controller/runtime.hpp>
#include <component/gamepad/controller/engine/icons.hpp>
#include <component/engine/console/command.hpp>

namespace gamepad::unstable::controller::engine
{
  void register_commands (const context& ctx, runtime& rt)
  {
    command::add ("controller_status", [&rt]
    {
      const context report = rt.make_context ();
      report.report (severity::info, facility::engine, errc::none,
        std::to_string (rt.device_count ()) + " device(s) bound; input source is " +
        (rt.keys ().in_use () ? "controller" : "keyboard and mouse") +
        "; ui_controllerIcons=" + read(rt.dvars().controller_icons, "auto") +
        "; resolved icons=" + (displayed_icon_family(rt) == mapping::glyph_family::xbox ? "xbox" : "ps3"));
    });
    command::add ("bindgpbuttonsconfigs", [&rt] { rt.binds ().reapply_layout (); });
    command::add ("bindgpsticksconfigs", [&rt]
    {
      rt.make_context ().report (severity::info, facility::engine, errc::none,
        string ("stick layout: ") + read (rt.dvars ().sticks_config, "thumbstick_default"));
    });
    ctx.report (severity::info, facility::engine, errc::none, "controller commands registered");
  }
}
