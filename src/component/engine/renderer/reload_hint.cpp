#include <std_include.hpp>
#include <loader/component_loader.hpp>
#include <component/engine/zones/fastfiles.hpp>
#include <component/utils/scheduler.hpp>
#include <utils/memory.hpp>

namespace reload_hint
{
  namespace
  {
    // QoS PC 1.1 ownerdraw dispatcher 102AC4E0, case 123, calls the native
    // low-ammo warning at 10286DB0. The shipped weaponinfo menu omits this
    // item. Restore only its presentation, not weapon/ammo or reload logic.
    utils::memory::allocator allocations;

    void prepare()
    {
      fastfiles::enum_assets(game::ASSET_TYPE_MENU, [](game::XAssetHeader header)
      {
        auto* menu = header.menu;
        if (!menu || !menu->window.name || std::strcmp(menu->window.name, "weaponinfo")
            || !menu->items || menu->itemCount <= 0 || menu->itemCount >= 512) return;
        game::itemDef_s* source = nullptr;
        for (int i = 0; i < menu->itemCount; ++i)
        {
          const auto* item = menu->items[i];
          if (!item) continue;
          if (item->window.ownerDraw == 123) return;
          if (item->window.ownerDraw == 119) source = menu->items[i];
        }
        if (!source) return;
        auto* hint = allocations.allocate<game::itemDef_s>();
        *hint = *source;
        hint->window.name = "cslReloadHint";
        hint->window.ownerDraw = 123;
        hint->window.ownerDrawFlags = 0;
        hint->window.rect = hint->window.rectClient = {-100.0f, 60.0f, 200.0f, 20.0f, 2, 2};
        hint->window.background = nullptr;
        hint->window.foreColor[3] = 1.0f;
        hint->foreColorAlphaExp = {};
        hint->rectXExp = hint->rectYExp = hint->rectWExp = hint->rectHExp = {};
        hint->textalignx = hint->textaligny = 0.0f;
        hint->textAlignMode = 1; // native ITEM_ALIGN_CENTER
        hint->textscale = 0.3f;
        hint->textStyle = 3;
        hint->text = nullptr;
        hint->textExp = {};
        hint->materialExp = {};
        hint->typeData.data = nullptr;
        hint->parent = menu;
        // Retain the ammo item's native HUD visibility expression. Never free
        // the DB-owned pointer array; client allocations outlive zone unloads.
        auto** items = allocations.allocate_array<game::itemDef_s*>(menu->itemCount + 1);
        std::copy_n(menu->items, menu->itemCount, items);
        items[menu->itemCount] = hint;
        menu->items = items;
        ++menu->itemCount;
      }, false);
    }
  }

  class component final : public component_interface
  {
  public:
    void post_load() override
    {
      scheduler::loop(prepare, scheduler::pipeline::main, 1000ms);
    }
  };
}

REGISTER_COMPONENT(reload_hint::component)
