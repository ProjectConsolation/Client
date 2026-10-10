#include <std_include.hpp>

#include "loader/component_loader.hpp"
#include "component/gamepad/gamepad.hpp"
#include "component/gamepad/controller/runtime.hpp"
#include "component/gamepad/controller/engine/hook.hpp"
#include "component/gamepad/controller/engine/import.hpp"
#include "component/gamepad/controller/mapping/key.hpp"
#include "component/utils/scheduler.hpp"

#include <utils/flags.hpp>

namespace controller_component
{
  namespace controller = gamepad::unstable::controller;
  std::unique_ptr<controller::runtime> runtime;

  class component final : public component_interface
  {
  public:
    void post_load () override
    {
      if (utils::flags::has_flag ("no_controller"))
        return;

      runtime = std::make_unique<controller::runtime> (
#ifdef DEBUG
        true
#else
        false
#endif
      );

      controller::engine::install (*runtime);

      scheduler::once ([]
      {
        if (runtime) runtime->engine_ready ();
      }, scheduler::main, 100ms);

      scheduler::loop ([]
      {
        if (runtime) runtime->frame ();
      }, scheduler::main, 16ms);

      scheduler::on_shutdown ([]
      {
        if (runtime) runtime->keys ().release_all ();
      });
    }
  };
}

namespace gamepad
{
  bool is_controller_active ()
  {
    return controller_component::runtime && controller_component::runtime->driving ();
  }

  bool should_hide_cursor () { return is_controller_active (); }

  void note_mouse_activity ()
  {
    if (controller_component::runtime)
    {
      controller_component::runtime->keys ().note_other_input ();
      controller_component::runtime->view ().idle ();
    }
  }

  void note_key_activity(int key, bool down)
  {
    if (controller_component::runtime && !unstable::controller::engine::emitting_controller_key()
        && !unstable::controller::mapping::is_controller_key(key))
    {
      controller_component::runtime->keys().note_pc_key(key, down);
      if (down) controller_component::runtime->view().idle();
    }
  }
}

REGISTER_COMPONENT(controller_component::component)
