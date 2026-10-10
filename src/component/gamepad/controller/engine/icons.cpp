#include <std_include.hpp>
#include "icons.hpp"
#include <component/gamepad/controller/runtime.hpp>
#include <component/gamepad/controller/mapping/icon_text.hpp>
#include <component/engine/zones/fastfiles.hpp>
#include <component/utils/scheduler.hpp>
#include <utils/hook.hpp>
#include <atomic>

namespace gamepad::unstable::controller::engine
{
  namespace
  {
    static_assert(sizeof(game::Font_s) == 24 && offsetof(game::Font_s, pixelHeight) == 4);
    static_assert(sizeof(game::Glyph) == 24 && offsetof(game::Glyph, dx) == 4);
    namespace text_icon = mapping::icon_text;
    std::array<std::atomic<game::Material*>, text_icon::count> materials{};
    utils::hook::detour draw_icon_hook, text_width_hook, localized_key_hook;
    void* draw_icon_original{};

    void prepare_icons()
    {
      // Native 2D materials/images live in the resident common_consolation zone.
      // Resolve on the main thread; the backend only reads published handles.
      static const auto names = []
      {
        std::array<std::string, text_icon::count> result;
        for (size_t i = 0; i < result.size(); ++i)
          result[i] = std::string("qos_controller_")
            + (i < text_icon::button_count ? "xbox_" : "ps3_")
            + text_icon::names[i % text_icon::button_count];
        return result;
      }();
      std::array<game::Material*, text_icon::count> linked{};
      // Verified QoS 103DFCA0 always balances its reader lock, including an
      // empty inventory. Do not poll missing assets through 103E1EE0: its
      // create-default=false material-miss path returns with the writer lock held.
      fastfiles::enum_assets(game::ASSET_TYPE_MATERIAL, [&](game::XAssetHeader header)
      {
        auto* material = header.material;
        if (!material || !material->name || std::strncmp(material->name, "qos_controller_", 15) != 0
            || !material->techniqueSet || material->textureCount != 1 || !material->textureTable) return;
        for (size_t i = 0; i < names.size(); ++i)
          if (names[i] == material->name) { linked[i] = material; break; }
      }, false);
      for (size_t i = 0; i < materials.size(); ++i)
        materials[i].store(linked[i], std::memory_order_release);
    }

    // ESI points just past '^'. -1 delegates stock pointer tokens unchanged;
    // a recognized but unavailable private handle is skipped, never dereferenced.
    uintptr_t __cdecl resolve_icon(const char* token) noexcept
    {
      if (token[0] != 1 || token[1] != 48 || token[2] != 48
          || token[3] != 'C' || token[4] != 'G' || token[5] != 'I') return ~uintptr_t{};
      const unsigned index = static_cast<unsigned char>(token[6]) - static_cast<unsigned>('A');
      if (index >= materials.size()) return 0;
      return reinterpret_cast<uintptr_t>(materials[index].load(std::memory_order_acquire));
    }

    // QoS PC 1.1 RB_DrawHudIcon (103B7D90): font EAX, token ESI, seven
    // caller-cleaned stack args, advance in XMM0. The native renderer consumes
    // a pointer at token+3. Only this adapter converts our bounded handle into
    // that pointer; binary pointer bytes never pass through strlen/Com_vsprintf.
    __declspec(naked) void draw_icon_stub()
    {
      __asm {
        push ebp
        mov ebp, esp
        sub esp, 8
        push ebx
        push esi
        push edi
        mov ebx, eax
        push esi
        call resolve_icon
        add esp, 4
        cmp eax, -1
        je native_token
        test eax, eax
        jz unavailable
        mov dword ptr [ebp - 8], 00303001h
        mov [ebp - 5], eax
        lea esi, [ebp - 8]
        // Preserve alpha, not the ^2 binding-text tint: button artwork owns RGB.
        mov eax, [ebp + 32]
        and eax, 0FF000000h
        or eax, 00FFFFFFh
        push eax
        push [ebp + 28]
        push [ebp + 24]
        push [ebp + 20]
        push [ebp + 16]
        push [ebp + 12]
        push [ebp + 8]
        mov eax, ebx
        call dword ptr [draw_icon_original]
        add esp, 28
        jmp finished
      unavailable:
        xorps xmm0, xmm0
      finished:
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
      native_token:
        mov eax, ebx
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        jmp dword ptr [draw_icon_original]
      }
    }

    unsigned decode_letter(const char* text, int& used)
    {
      const auto target = game::game_offset(0x103C9940);
      const unsigned first = static_cast<unsigned char>(text[0]);
      const unsigned second = static_cast<unsigned char>(text[1]);
      int* count = &used;
      unsigned letter;
      __asm {
        push esi
        push edi
        mov edx, first
        mov esi, second
        mov edi, count
        call target
        mov letter, eax
        pop edi
        pop esi
      }
      return letter;
    }

    unsigned glyph_advance(unsigned letter, game::Font_s* font)
    {
      const auto target = game::game_offset(0x1037CCF0);
      game::Glyph* glyph;
      __asm {
        push edi
        push font
        mov edi, letter
        call target
        add esp, 4
        mov glyph, eax
        pop edi
      }
      return static_cast<unsigned char>(glyph->dx);
    }

    int __cdecl text_width(const char* text, int max_chars, game::Font_s* font)
    {
      if (!text || !font) return 0;
      const std::string_view input(text);
      if (input.find(std::string_view("^\x01\x30\x30" "CGI", 7)) == input.npos)
        return text_width_hook.invoke<int>(text, max_chars, font);
      // Match native R_TextWidth's color, newline, localized decoder and count
      // behavior. Unlike its PC path, count a controller icon as ONE character.
      int widest = 0, line = 0, count = 0;
      if (max_chars <= 0) max_chars = INT_MAX;
      size_t offset = 0;
      while (offset < input.size() && count < max_chars)
      {
        if (text_icon::decode(input.substr(offset)) >= 0)
        {
          line += text_icon::advance(font->pixelHeight);
          offset += text_icon::token_size;
          ++count;
        }
        else
        {
          int used = 1;
          const auto letter = decode_letter(text + offset, used);
          offset += static_cast<size_t>(used);
          if (letter == '\n' || letter == '\r') { line = 0; continue; }
          if (letter == '^' && offset < input.size()
              && input[offset] >= '0' && input[offset] <= '9') { ++offset; continue; }
          line += static_cast<int>(glyph_advance(letter, font));
          ++count;
        }
        widest = std::max(widest, line);
      }
      return widest;
    }

    // QoS PC 1.1 CG_HudElem text sizing at 102BC27A/102BC2FA keeps its
    // scale in XMM0 across R_TextWidth. The integer-only native implementation
    // preserves SSE registers; a normal C++ detour does not. Keep that stronger
    // native contract for ALL strings, including the non-icon delegation path.
    // Retire this adapter if native HUD sizing is replaced by a verified ABI.
    __declspec(naked) void text_width_stub()
    {
      __asm {
        push ebp
        mov ebp, esp
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        push [ebp + 16]
        push [ebp + 12]
        push [ebp + 8]
        call text_width
        add esp, 12
        movups xmm0, [esp]
        movups xmm1, [esp + 16]
        movups xmm2, [esp + 32]
        movups xmm3, [esp + 48]
        movups xmm4, [esp + 64]
        movups xmm5, [esp + 80]
        movups xmm6, [esp + 96]
        movups xmm7, [esp + 112]
        mov esp, ebp
        pop ebp
        ret
      }
    }

    const char* __cdecl localized_key(const char* text)
    {
      // QoS 102CED30 localizes the result of Key_KeynumToString through
      // cdecl 103C9F20 before formatting ^2%s^7. A private icon is already
      // display text, not an ASSET_TYPE_LOCALIZE_ENTRY name.
      if (text && text_icon::is_binding_token(text)) return text;
      return localized_key_hook.invoke<const char*>(text);
    }
  }

  mapping::glyph_family displayed_icon_family(const runtime& rt) noexcept
  {
    const int choice = mapping::icon_text::override_index(read(rt.dvars().controller_icons, "auto"));
    const optional<mapping::glyph_family> forced = choice < 0 ? nullopt
      : optional<mapping::glyph_family>(choice == 0 ? mapping::glyph_family::xbox
                                                   : mapping::glyph_family::playstation);
    return mapping::glyph_family_for(rt.latest().family, forced);
  }

  const char* displayed_button_icon(const runtime& rt, mapping::engine_key key) noexcept
  {
    const auto* token = mapping::glyph_for(key, displayed_icon_family(rt));
    if (!token) return nullptr;
    const int index = mapping::icon_text::decode(std::string_view(token, mapping::icon_text::token_size));
    return index >= 0 && materials[index].load(std::memory_order_acquire) ? token : nullptr;
  }

  void install_prompt_icons()
  {
    draw_icon_hook.create(game::game_offset(0x103B7D90), draw_icon_stub);
    draw_icon_original = draw_icon_hook.get_original();
    text_width_hook.create(game::game_offset(0x1037CFA0), text_width_stub);
    localized_key_hook.create(game::game_offset(0x103C9F20), localized_key);
    scheduler::loop(prepare_icons, scheduler::main, 1000ms);
  }
}
