#include <std_include.hpp>
#include "netmove.hpp"
#include "import.hpp"
#include <game/protocol.hpp>
#include <utils/hook.hpp>

namespace gamepad::unstable::controller::engine
{
  namespace
  {
    uintptr_t write_return, read_short_return, read_full_return;
    static_assert(offsetof(usercmd_s, forwardmove) == 0x1C);
    static_assert(offsetof(usercmd_s, rightmove) == 0x1E);
    static_assert(sizeof(usercmd_s) == 44);

    // QoS writer: EBP to, EDI from; var_4 ESP+0xC, arg_8 ESP+0x1C.
    // Native delta helper supplies presence and XOR. Pack plain bytes here.
    __declspec(naked) void write_axes_stub()
    {
      __asm {
        movzx eax, byte ptr [ebp + 1Ch]
        mov ah, byte ptr [ebp + 1Eh]
        mov [esp + 0Ch], eax
        movzx eax, byte ptr [edi + 1Ch]
        mov ah, byte ptr [edi + 1Eh]
        mov [esp + 1Ch], eax
        jmp dword ptr [write_return]
      }
    }

    void __cdecl read_axes(void* msg, int key, const usercmd_s* from, usercmd_s* to) noexcept
    {
      const auto read_bit = game::game_offset(0x103EEDE0);
      const auto read_bits = game::game_offset(0x103EEE50);
      int present, packed = 0;
      __asm {
        mov edx, msg
        call read_bit
        mov present, eax
      }
      move_delta axes {from->forwardmove, from->rightmove};
      if (present == 1)
      {
        __asm {
          push esi
          mov esi, msg
          push 16
          call read_bits
          add esp, 4
          pop esi
          mov packed, eax
        }
        if (packed >= 0) axes = unpack_move(static_cast<uint16_t>(packed), key);
      }
      // On truncation native MSG overflow stays set; retain the baseline.
      to->forwardmove = axes.forward;
      to->rightmove = axes.right;
    }

    __declspec(naked) void read_short_stub()
    {
      __asm {
        pushad
        push edi
        push ebx
        push ebp
        push esi
        call read_axes
        add esp, 16
        popad
        jmp dword ptr [read_short_return]
      }
    }
    __declspec(naked) void read_full_stub()
    {
      __asm {
        pushad
        push edi
        push ebx
        push ebp
        push esi
        call read_axes
        add esp, 16
        popad
        jmp dword ptr [read_full_return]
      }
    }

    void require_bytes(uintptr_t address, std::initializer_list<unsigned char> expected)
    {
      if (std::memcmp(reinterpret_cast<const void*>(game::game_offset(address)),
                      expected.begin(), expected.size()) != 0)
        throw std::runtime_error("QoS analog protocol patch: unexpected binary bytes");
    }
  }

  void install_analog_protocol()
  {
    // QoS PC IDA + KisakCOD msg_mp.cpp verified. Adapt IW4x's extension,
    // retaining QoS-only loadoutClass, melee fields, timestamps and key changes.
    // Validate every site before changing anything, including protocol gates.
    require_bytes(0x103EF865, {0x0F,0xBE,0x45,0x1C});
    require_bytes(0x103F027B, {0x0F,0xBE,0x43,0x1C});
    require_bytes(0x103F03A9, {0x0F,0xBE,0x43,0x1C});
    require_bytes(0x103EF9CE, {0x6A,4});
    require_bytes(0x103EFA44, {0x6A,4});
    const uintptr_t push_sites[] {0x102EFDD5,0x102F72F9,0x102F72FB,
      0x102F72FD,0x102F929C,0x1031EF6E,0x102F0A4E};
    for (auto site : push_sites) require_bytes(site, {0x6A,47});
    require_bytes(0x102EFDA8, {0x83,0xFE,47});
    require_bytes(0x1030ED39, {0xB8,47,0,0,0});

    write_return = game::game_offset(0x103EF8E9);
    read_short_return = game::game_offset(0x103F0310);
    read_full_return = game::game_offset(0x103F043E);
    utils::hook::jump(game::game_offset(0x103EF865), write_axes_stub);
    utils::hook::jump(game::game_offset(0x103F027B), read_short_stub);
    utils::hook::jump(game::game_offset(0x103F03A9), read_full_stub);
    utils::hook::set<unsigned char>(game::game_offset(0x103EF9CE + 1), 16);
    utils::hook::set<unsigned char>(game::game_offset(0x103EFA44 + 1), 16);
    for (auto site : push_sites)
      utils::hook::set<unsigned char>(game::game_offset(site + 1), game::consolation_protocol);
    utils::hook::set<unsigned char>(game::game_offset(0x102EFDA8 + 2), game::consolation_protocol);
    utils::hook::set<int>(game::game_offset(0x1030ED39 + 1), game::consolation_protocol);
  }
}
