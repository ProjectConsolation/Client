#include <std_include.hpp>
#include "ingame_settings.hpp"
#include <game/game.hpp>
#include <game/dvars.hpp>
#include <component/engine/console/command.hpp>
#include <component/engine/console/console.hpp>
#include <component/engine/zones/fastfiles.hpp>
#include <utils/hook.hpp>
#include <utils/memory.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <unordered_map>

namespace ingame_settings
{
  namespace
  {
    utils::memory::allocator memory;
    std::unordered_map<std::string, std::array<char, 128>> values;
    std::unordered_map<std::string, std::string> video, original_video;
    std::unordered_map<std::string, float> last_slider_values;
    game::menuDef_t* last_top{};
    constexpr const char* video_dvars[] = {
      "r_mode", "r_rendererPreference", "sf_bx_alt_vid_quality", "r_fullscreen", "r_borderless",
      "r_aaSamples", "r_texFilterAnisoMin", "r_texFilterAnisoMax", "r_vsync", "sm_enable",
      "r_dlightLimit", "r_picmip_manual", "r_picmip", "r_picmip_bump", "r_picmip_spec",
      "r_lodScaleParam", "fx_marks", "vid_xpos", "vid_ypos"};
    constexpr int fps_presets[] = {30, 60, 85, 125, 250, 333, 0};

    std::string current_value(game::dvar_s* dvar)
    {
      const auto type = static_cast<unsigned char>(dvar->type);
      if (type == game::DVAR_TYPE_ENUM)
      {
        const auto index = dvar->current.integer;
        return index >= 0 && index < dvar->domain.enumeration.stringCount
          ? dvar->domain.enumeration.strings[index] : "";
      }
      if (type == game::DVAR_TYPE_STRING) return dvar->current.string ? dvar->current.string : "";
      if (type == game::DVAR_TYPE_BOOL) return dvar->current.enabled ? "1" : "0";
      if (type == game::DVAR_TYPE_INT) return std::to_string(dvar->current.integer);
      return dvars::Dvar_ValueToString(dvar, dvar->current);
    }

    void begin_video()
    {
      video.clear();
      for (const auto* key : video_dvars)
        if (auto* dvar = game::Dvar_FindVar(key)) video[key] = current_value(dvar);
      original_video = video;
    }

    void cycle(const char* key, std::initializer_list<const char*> choices)
    {
      auto found = video.find(key);
      if (found == video.end() || choices.size() == 0) return;
      auto value = std::find(choices.begin(), choices.end(), found->second);
      if (value != choices.end()) ++value;
      found->second = value == choices.end() ? *choices.begin() : *value;
    }

    void cycle_enum(const char* key)
    {
      auto* dvar = game::Dvar_FindVar(key);
      if (!dvar || static_cast<unsigned char>(dvar->type) != game::DVAR_TYPE_ENUM
          || !video.contains(key) || dvar->domain.enumeration.stringCount <= 0) return;
      const auto& domain = dvar->domain.enumeration;
      int index = 0;
      while (index < domain.stringCount && video[key] != domain.strings[index]) ++index;
      video[key] = domain.strings[(index + 1) % domain.stringCount];
    }

    const char* copy(const std::string& text)
    {
      auto* result = memory.allocate_array<char>(text.size() + 1);
      std::memcpy(result, text.c_str(), text.size() + 1);
      return result;
    }

    bool named(const game::menuDef_t* menu, const char* name)
    {
      return menu && menu->window.name && !std::strcmp(menu->window.name, name);
    }

    game::menuDef_t* find(unsigned char* context, const char* name)
    {
      const auto count = *reinterpret_cast<const int*>(context + 2104);
      if (count < 0 || count > 512) return nullptr;
      auto** menus = reinterpret_cast<game::menuDef_t**>(context + 56);
      for (int i = 0; i < count; ++i) if (named(menus[i], name)) return menus[i];
      return nullptr;
    }

    bool extended(game::menuDef_t* menu)
    {
      if (!menu || !menu->items || menu->itemCount < 0 || menu->itemCount > 512) return true;
      for (int i = 0; i < menu->itemCount; ++i)
        if (menu->items[i]->window.name && !std::strcmp(menu->items[i]->window.name, "cslNativeSettings")) return true;
      return false;
    }

    void append(game::menuDef_t* menu, const std::vector<game::itemDef_s*>& additions)
    {
      auto** items = memory.allocate_array<game::itemDef_s*>(menu->itemCount + additions.size());
      std::copy_n(menu->items, menu->itemCount, items);
      std::copy(additions.begin(), additions.end(), items + menu->itemCount);
      // Database-owned arrays must never be freed. These clones are main-thread
      // UI allocations and survive until process exit, like parsed disk menus.
      menu->items = items;
      menu->itemCount += static_cast<int>(additions.size());
    }

    game::itemDef_s* item(game::menuDef_t* menu, const game::itemDef_s* source,
      float y, const char* caption, const char* action)
    {
      auto* result = memory.allocate<game::itemDef_s>();
      *result = *source;
      result->parent = menu;
      result->window.name = "cslNativeSettings";
      result->window.group = nullptr;
      result->window.rect = result->window.rectClient = {50, y, 440, 20, 0, 0};
      result->window.style = 1;
      result->window.backColor[3] = 0.15f;
      result->window.dynamicFlags[0] = 4;
      result->type = 1;
      result->dataType = 0;
      result->typeData.data = nullptr;
      result->window.ownerDraw = 0;
      result->window.ownerDrawFlags = 0;
      result->visibleExp = result->textExp = result->materialExp = {};
      result->rectXExp = result->rectYExp = result->rectWExp = result->rectHExp = {};
      result->foreColorAlphaExp = {};
      result->text = caption;
      result->textRect[0] = result->textRect[1] = result->textRect[2] = result->textRect[3] = {};
      result->textAlignMode = 0;
      result->textalignx = 8;
      result->textaligny = 15;
      result->textscale = 0.26f;
      result->action = action;
      result->onAccept = nullptr;
      result->onFocus = "play mouse_over ;";
      result->leaveFocus = nullptr;
      result->mouseEnterText = result->mouseExitText = nullptr;
      result->mouseEnter = result->mouseExit = nullptr;
      result->dvar = result->dvarTest = result->enableDvar = nullptr;
      result->dvarFlags = 0;
      return result;
    }

    void row(std::vector<game::itemDef_s*>& items, game::menuDef_t* menu,
      const game::itemDef_s* source, float y, const char* caption,
      const char* dvar, const char* action, float minimum = 0, float maximum = 0)
    {
      auto* control = item(menu, source, y, caption, action);
      if (maximum > minimum)
      {
        control->type = 10;
        control->dataType = 4;
        control->dvar = dvar;
        auto* range = memory.allocate<game::editFieldDef_s>();
        *range = {};
        range->minVal = minimum;
        range->maxVal = maximum;
        range->range = maximum - minimum;
        control->typeData.editField = range;
        // Native sensitivity uses this same left-label/right-slider geometry.
        control->textalignx = 220;
        control->textAlignMode = 2;
      }
      items.push_back(control);
      if (!dvar) return;
      auto* value = item(menu, source, y, "", nullptr);
      value->window.name = copy(std::string("cslLive_") + dvar);
      value->window.style = 0;
      value->window.rect.x = value->window.rectClient.x = 505;
      value->window.rect.w = value->window.rectClient.w = 130;
      value->type = 0;
      value->textalignx = 0;
      value->text = values[dvar].data();
      items.push_back(value);
    }

    game::menuDef_t* page(unsigned char* context, game::menuDef_t* stock,
      const char* name, const char* title, const char* back)
    {
      auto& count = *reinterpret_cast<int*>(context + 2104);
      if (count >= 512 || stock->itemCount < 22 || !stock->items) return nullptr;
      auto* menu = memory.allocate<game::menuDef_t>();
      *menu = *stock;
      menu->window.name = name;
      menu->onESC = copy(std::string("close ") + name + " ;");
      menu->onClose = copy(std::string("open ") + back + " ; exec updategamerprofile ;");
      menu->onOpen = nullptr;
      menu->itemCount = 16; // Retail header, backdrop, separators and BACK.
      menu->items = memory.allocate_array<game::itemDef_s*>(16);
      for (int i = 0; i < 16; ++i)
      {
        menu->items[i] = memory.allocate<game::itemDef_s>();
        *menu->items[i] = *stock->items[i];
        menu->items[i]->parent = menu;
        auto* decoration = menu->items[i];
        if (decoration->window.rect.y >= 45 && decoration->window.rect.y < 90
            && (decoration->text || decoration->textExp.numEntries))
        {
          decoration->textExp = {};
          decoration->text = title;
        }
        if (i == 14) decoration->action = menu->onESC;
      }
      std::fill(std::begin(menu->cursorItem), std::end(menu->cursorItem), -1);
      reinterpret_cast<game::menuDef_t**>(context + 56)[count++] = menu;
      return menu;
    }

    void prepare(unsigned char* context)
    {
      auto* gameplay = find(context, "ingame_options");
      auto* controls = find(context, "ingame_controls");
      auto* pause = find(context, "ingame");
      if (!gameplay || gameplay->itemCount < 22 || extended(gameplay)) return;
      const auto* source = gameplay->items[20];
      std::vector<game::itemDef_s*> additions;
      row(additions, gameplay, source, 246, "FOV", "cg_fov", nullptr, 65, 160);
      if (!game::Dvar_FindVar("ui_cslFpsSlider"))
        dvars::Dvar_RegisterFloat("ui_cslFpsSlider", "In-game FPS preset slider index (menu transport only).", 2, 0, 6, game::dvar_flags::none);
      row(additions, gameplay, source, 268, "MAXIMUM FPS", "ui_cslFpsSlider", nullptr, 0, 6);
      additions.back()->window.name = "cslLive_com_maxfps";
      row(additions, gameplay, source, 290, "HORIZONTAL SAFE AREA", "safeArea_horizontal", nullptr, 0, 1);
      row(additions, gameplay, source, 312, "VERTICAL SAFE AREA", "safeArea_vertical", nullptr, 0, 1);
      row(additions, gameplay, source, 334, "LEGACY WEAPON SWITCHING (HOST)", "pm_legacySwitching",
        "exec \"toggle pm_legacySwitching\" ;");
      additions.push_back(item(gameplay, source, 360,
        "UNLIMITED FPS: GOING OVER 333 MAY BREAK PHYSICS", nullptr));
      additions.back()->type = 0;
      additions.back()->window.name = "cslFpsWarning";
      additions.back()->window.style = 0;
      additions.back()->textscale = 0.2f;
      append(gameplay, additions);

      if (controls && !extended(controls))
      {
        // Place CONTROLLER below MULTIPLAYER, before RESET TO DEFAULT.
        for (int i = 46; i < controls->itemCount; ++i)
        {
          controls->items[i]->window.rect.y += 24;
          controls->items[i]->window.rectClient.y += 24;
        }
        append(controls, {item(controls, source, 270, "CONTROLLER",
          "open ingame_controller ;")});
      }
      if (auto* controller = page(context, gameplay, "ingame_controller", "CONTROLLER", "ingame_controls"))
      {
        additions.clear();
        row(additions, controller, source, 126, "CONTROLLER", "gpad_enabled", "exec \"toggle gpad_enabled\" ;");
        row(additions, controller, source, 150, "LOOK SENSITIVITY", "gpad_sensitivity", "exec \"toggle gpad_sensitivity 1 2 3 4 5 6 7 8 9 10\" ;");
        row(additions, controller, source, 174, "LOOK INVERSION", "input_invertPitch", "exec \"toggle input_invertPitch\" ;");
        row(additions, controller, source, 198, "BUTTON LAYOUT", "gpad_buttonsConfig", "exec \"toggle gpad_buttonsConfig buttons_default buttons_tactical buttons_default_alt buttons_tactical_alt\" ; exec bindgpbuttonsconfigs ;");
        row(additions, controller, source, 222, "STICK LAYOUT", "gpad_sticksConfig", "exec \"toggle gpad_sticksConfig thumbstick_default thumbstick_southpaw thumbstick_legacy thumbstick_legacysouthpaw\" ; exec bindgpsticksconfigs ;");
        row(additions, controller, source, 246, "CONTROLLER ICONS", "ui_controllerIcons", "exec \"toggle ui_controllerIcons auto xbox ps3\" ;");
        row(additions, controller, source, 270, "RUMBLE", "gpad_rumble", "exec \"toggle gpad_rumble\" ;");
        row(additions, controller, source, 294, "INNER DEADZONE", "gpad_stick_deadzone_min", nullptr, 0, 1);
        row(additions, controller, source, 318, "TRIGGER DEADZONE", "gpad_button_deadzone", nullptr, 0, 1);
        row(additions, controller, source, 342, "RESET CONTROLLER", nullptr,
          "setdvar gpad_enabled 1 ; setdvar gpad_sensitivity 5 ; setdvar input_invertPitch 0 ; setdvar gpad_buttonsConfig buttons_default_alt ; setdvar gpad_sticksConfig thumbstick_default ; setdvar ui_controllerIcons auto ; setdvar gpad_rumble 1 ; setdvar gpad_stick_deadzone_min 0.2 ; setdvar gpad_button_deadzone 0.13 ; exec bindgpbuttonsconfigs ; exec bindgpsticksconfigs ;");
        row(additions, controller, source, 366, "CONTROLLER PICTURE", nullptr, "open ingame_controller_picture ;");
        append(controller, additions);
      }
      if (auto* picture = page(context, gameplay, "ingame_controller_picture", "CONTROLLER", "ingame_controller"))
      {
        auto* image = item(picture, source, 126, nullptr, nullptr);
        image->window.name = "cslControllerPicture";
        image->type = 0;
        image->window.rect = image->window.rectClient = {50, 126, 540, 270, 0, 0};
        image->window.background = nullptr;
        image->window.style = 0; // Never draw a missing/default asset.
        std::fill(std::begin(image->window.backColor), std::end(image->window.backColor), 1.0f);
        append(picture, {image});
      }
      if (auto* graphics = page(context, gameplay, "ingame_graphics", "GRAPHICS", "ingame"))
      {
        graphics->onOpen = "exec \"cslIngameVideo begin\" ;";
        graphics->onESC = "exec \"cslIngameVideo exit\" ;";
        graphics->items[14]->action = graphics->onESC;
        additions.clear();
        row(additions, graphics, source, 126, "VIDEO MODE", "r_mode", "exec \"cslIngameVideo mode\" ;");
        row(additions, graphics, source, 148, "VIDEO QUALITY", "r_rendererPreference", "exec \"cslIngameVideo quality\" ;");
        row(additions, graphics, source, 170, "DISPLAY MODE", "r_borderless", "exec \"cslIngameVideo display\" ;");
        row(additions, graphics, source, 192, "ANTI-ALIASING", "r_aaSamples", "exec \"cslIngameVideo aa\" ;");
        row(additions, graphics, source, 214, "ANISOTROPIC FILTERING", "r_texFilterAnisoMax", "exec \"cslIngameVideo aniso\" ;");
        // QoS has one real presentation switch; sf_revertvalues_r_vsync is
        // Scaleform rollback storage, not another renderer toggle.
        row(additions, graphics, source, 236, "V-SYNC", "r_vsync", "exec \"cslIngameVideo vsync\" ;");
        row(additions, graphics, source, 258, "SHADOWS", "sm_enable", "exec \"cslIngameVideo shadows\" ;");
        row(additions, graphics, source, 280, "DYNAMIC LIGHTS", "r_dlightLimit", "exec \"cslIngameVideo lights\" ;");
        row(additions, graphics, source, 302, "TEXTURE DETAILS", "r_picmip", "exec \"cslIngameVideo textures\" ;");
        row(additions, graphics, source, 324, "MODEL DETAIL", "r_lodScaleParam", "exec \"cslIngameVideo models\" ;");
        row(additions, graphics, source, 346, "BULLET IMPACTS", "fx_marks", "exec \"cslIngameVideo marks\" ;");
        row(additions, graphics, source, 368, "RESET VIDEO POSITION", nullptr, "exec \"cslIngameVideo position\" ;");
        row(additions, graphics, source, 390, "APPLY GRAPHICS", nullptr, "exec \"cslIngameVideo apply\" ;");
        append(graphics, additions);
        if (auto* confirm = page(context, gameplay, "ingame_video_discard", "EXIT WITHOUT SAVING?", "ingame_graphics"))
        {
          confirm->onClose = nullptr;
          append(confirm, {item(confirm, source, 150, "YES", "exec \"cslIngameVideo discard\" ;"),
            item(confirm, source, 174, "NO", "close ingame_video_discard ;")});
        }
        if (pause && !extended(pause))
          append(pause, {item(pause, source, 222, "GRAPHICS", "open ingame_graphics ; close ingame ;")});
      }
    }

    void refresh(unsigned char* context)
    {
      const auto count = *reinterpret_cast<const int*>(context + 2172);
      if (count < 0 || count > 16) return;
      auto* top = count ? reinterpret_cast<game::menuDef_t**>(context + 2108)[count - 1] : nullptr;
      if (top != last_top) { last_slider_values.clear(); last_top = top; }
      for (int i = 0; i < count; ++i)
      {
        auto* menu = reinterpret_cast<game::menuDef_t**>(context + 2108)[i];
        if (!menu || menu->itemCount < 0 || menu->itemCount > 512) continue;
        for (int j = 0; j < menu->itemCount; ++j)
        {
          auto* item = menu->items[j];
          const auto* name = item->window.name;
          if (name && !std::strcmp(name, "cslControllerPicture"))
          {
            const auto* family = game::Dvar_FindVar("ui_controllerIconFamily");
            const auto* override = game::Dvar_FindVar("ui_controllerIcons");
            const char* selected = family && family->current.string ? family->current.string : "xbox";
            if (override && override->current.string && (!std::strcmp(override->current.string, "xbox")
                || !std::strcmp(override->current.string, "ps3"))) selected = override->current.string;
            const auto* material_name = !std::strcmp(selected, "ps3") ? "ps3_controller_ingame" : "xenon_controller_ingame";
            // Enumerate resident materials; the native no-default lookup's
            // material-miss path can leave the DB writer lock held.
            game::Material* material = nullptr;
            fastfiles::enum_assets(game::ASSET_TYPE_MATERIAL, [&](game::XAssetHeader header) {
              if (header.material && header.material->name && !std::strcmp(header.material->name, material_name)) material = header.material;
            }, false);
            item->window.background = material;
            item->window.style = material ? 3 : 0;
            continue;
          }
          if (name && !std::strcmp(name, "cslFpsWarning"))
          {
            const auto* fps = game::Dvar_FindVar("com_maxfps");
            item->window.dynamicFlags[0] = fps && fps->current.integer == 0 ? 4 : 0;
            continue;
          }
          if (!name || std::strncmp(name, "cslLive_", 8)) continue;
          const auto* key = name + 8;
          auto* dvar = game::Dvar_FindVar(key);
          auto& output = values[key];
          if (!dvar) { strcpy_s(output.data(), output.size(), "UNAVAILABLE"); continue; }
          if (!std::strcmp(key, "cg_fov") || !std::strncmp(key, "safeArea_", 9))
          {
            const bool safe = key[0] == 's';
            const auto current = dvar->current.value;
            const float step = safe ? 0.05f : 5.0f;
            const auto snapped = std::clamp(std::round(current / step) * step, safe ? 0.0f : 65.0f, safe ? 1.0f : 160.0f);
            const auto previous = last_slider_values.find(key);
            const bool changed = menu == top && previous != last_slider_values.end()
              && std::abs(previous->second - current) > 0.00001f;
            if (changed && std::abs(current - snapped) > 0.00001f)
              game::Dvar_SetFromStringByName(key, std::to_string(snapped).c_str());
            if (menu == top) last_slider_values[key] = changed ? snapped : current;
            sprintf_s(output.data(), output.size(), "%d", static_cast<int>(std::round((changed ? snapped : current) * (safe ? 100 : 1))));
          }
          else if (named(menu, "ingame_graphics") && video.contains(key))
          {
            std::string text = video[key];
            if (!std::strcmp(key, "r_borderless")) text = text == "1" ? "BORDERLESS FULLSCREEN"
              : video["r_fullscreen"] == "1" ? "FULLSCREEN" : "WINDOWED";
            else if (!std::strcmp(key, "r_vsync") || !std::strcmp(key, "sm_enable") || !std::strcmp(key, "fx_marks"))
              text = text == "1" ? "ENABLED" : "DISABLED";
            else if (!std::strcmp(key, "r_aaSamples") || !std::strcmp(key, "r_texFilterAnisoMax"))
              text = text == "1" ? "OFF" : text + "X";
            else if (!std::strcmp(key, "r_rendererPreference"))
              text = video["sf_bx_alt_vid_quality"] == "1" ? "LOW" : text;
            else if (!std::strcmp(key, "r_picmip"))
            {
              if (video["r_picmip_manual"] == "0") text = "AUTO";
              else if (text == "3") text = "LOW";
              else if (text == "2") text = "MEDIUM";
              else if (text == "1") text = "HIGH";
              else if (text == "0") text = "EXTRA";
              else text = "CUSTOM";
            }
            else if (!std::strcmp(key, "r_lodScaleParam"))
              text = text == "0" ? "LOW" : text == "1" ? "MEDIUM" : "HIGH";
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            strncpy_s(output.data(), output.size(), text.c_str(), _TRUNCATE);
          }
          else if (!std::strcmp(key, "com_maxfps"))
          {
            auto* slider = game::Dvar_FindVar("ui_cslFpsSlider");
            if (menu == top && slider)
            {
              const auto previous = last_slider_values.find("ui_cslFpsSlider");
              if (previous == last_slider_values.end())
              {
                int closest = 0;
                for (int preset = 1; preset < 7; ++preset)
                  if (std::abs(fps_presets[preset] - dvar->current.integer)
                      < std::abs(fps_presets[closest] - dvar->current.integer)) closest = preset;
                game::Dvar_SetFromStringByName("ui_cslFpsSlider", std::to_string(closest).c_str());
                last_slider_values["ui_cslFpsSlider"] = static_cast<float>(closest);
              }
              else if (std::abs(slider->current.value - previous->second) > 0.00001f)
              {
                const int preset = std::clamp(static_cast<int>(std::round(slider->current.value)), 0, 6);
                game::Dvar_SetFromStringByName("ui_cslFpsSlider", std::to_string(preset).c_str());
                game::Dvar_SetFromStringByName("com_maxfps", std::to_string(fps_presets[preset]).c_str());
                last_slider_values["ui_cslFpsSlider"] = static_cast<float>(preset);
              }
            }
            if (dvar->current.integer == 0) strcpy_s(output.data(), output.size(), "UNLIMITED");
            else sprintf_s(output.data(), output.size(), dvar->current.integer == 30 ? "%d (NATIVE)" : "%d", dvar->current.integer);
          }
          else if (static_cast<unsigned char>(dvar->type) == game::DVAR_TYPE_BOOL)
            strcpy_s(output.data(), output.size(), dvar->current.enabled ? "ENABLED" : "DISABLED");
          else
          {
            const auto* text = dvars::Dvar_ValueToString(dvar, dvar->current);
            strncpy_s(output.data(), output.size(), text ? text : "UNAVAILABLE", _TRUNCATE);
            for (auto& c : output) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          }
          item->text = output.data();
        }
      }
    }
  }

  void update(unsigned char* context)
  {
    if (context != reinterpret_cast<unsigned char*>(game::game_offset(0x113F1140))) return;
    prepare(context);
    refresh(context);
  }

  void register_commands()
  {
    command::add("cslIngameVideo", [](const command::params& args) {
      if (args.size() < 2) return;
      auto* context = reinterpret_cast<unsigned char*>(game::game_offset(0x113F1140));
      auto* graphics = find(context, "ingame_graphics");
      if (!graphics) return;
      const std::string action = args[1];
      const auto close = [&](const char* name) {
        if (auto* menu = find(context, name)) utils::hook::invoke<void>(game::game_offset(0x102CE070), context, menu);
      };
      if (action == "begin") { begin_video(); return; }
      if (action == "exit")
      {
        if (video != original_video)
          utils::hook::invoke<int>(game::game_offset(0x102D8B90), context, "ingame_video_discard");
        else close("ingame_graphics");
        return;
      }
      if (action == "discard")
      {
        video = original_video;
        close("ingame_video_discard");
        close("ingame_graphics");
        return;
      }
      if (action == "apply")
      {
        for (const auto& [key, value] : video)
          if (original_video.contains(key) && original_video[key] != value)
            game::Dvar_SetFromStringByName(key.c_str(), value.c_str());
        original_video = video;
        close("ingame_graphics");
        command::execute("vid_restart");
        return;
      }
      if (action == "mode") cycle_enum("r_mode");
      else if (action == "quality")
      {
        // Retail LOW is renderer[0]+altQuality=1; MEDIUM/HIGH enumerate
        // renderer preferences with altQuality=0, not just an enum toggle.
        auto* dvar = game::Dvar_FindVar("r_rendererPreference");
        if (dvar && static_cast<unsigned char>(dvar->type) == game::DVAR_TYPE_ENUM
            && dvar->domain.enumeration.stringCount > 0)
        {
          const auto& domain = dvar->domain.enumeration;
          if (video["sf_bx_alt_vid_quality"] == "1")
            video["sf_bx_alt_vid_quality"] = "0";
          else if (video["r_rendererPreference"] == domain.strings[domain.stringCount - 1])
          {
            video["r_rendererPreference"] = domain.strings[0];
            video["sf_bx_alt_vid_quality"] = "1";
          }
          else cycle_enum("r_rendererPreference");
          if (video["sf_bx_alt_vid_quality"] == "1"
              || video["r_rendererPreference"] == domain.strings[0]) video["sm_enable"] = "0";
        }
      }
      else if (action == "display")
      {
        if (video["r_borderless"] == "1") { video["r_borderless"] = "0"; video["r_fullscreen"] = "0"; }
        else if (video["r_fullscreen"] == "1") { video["r_borderless"] = "1"; video["r_fullscreen"] = "0"; }
        else { video["r_fullscreen"] = "1"; video["r_borderless"] = "0"; }
      }
      else if (action == "aa") cycle("r_aaSamples", {"1", "2", "4", "8", "16"});
      else if (action == "aniso") { cycle("r_texFilterAnisoMax", {"1", "2", "4", "8", "16"}); video["r_texFilterAnisoMin"] = video["r_texFilterAnisoMax"]; }
      else if (action == "vsync") cycle("r_vsync", {"0", "1"});
      else if (action == "shadows") cycle("sm_enable", {"0", "1"});
      else if (action == "lights") cycle("r_dlightLimit", {"0", "1", "2", "3", "4"});
      else if (action == "models") cycle("r_lodScaleParam", {"0", "1", "2"});
      else if (action == "marks") cycle("fx_marks", {"0", "1"});
      else if (action == "textures")
      {
        // Exact retail menuData AUTO/LOW/MEDIUM/HIGH/EXTRA presets.
        static constexpr int presets[][4] = {{0,0,0,0}, {1,3,2,2}, {1,2,1,1}, {1,1,0,0}, {1,0,0,0}};
        constexpr const char* keys[] = {"r_picmip_manual", "r_picmip", "r_picmip_bump", "r_picmip_spec"};
        int index = 0;
        for (; index < 5; ++index)
        {
          bool match = true;
          for (int i = 0; i < 4; ++i) match &= video[keys[i]] == std::to_string(presets[index][i]);
          if (match) break;
        }
        index = (index + 1) % 5;
        for (int i = 0; i < 4; ++i) video[keys[i]] = std::to_string(presets[index][i]);
      }
      else if (action == "position") { video["vid_xpos"] = "0"; video["vid_ypos"] = "0"; }
      else console::warn("cslIngameVideo: unknown menu action '%s'\n", action.c_str());
    });
  }
}
