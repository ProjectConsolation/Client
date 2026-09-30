#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

#include <unordered_set>

namespace renderer_state_validation
{
	namespace
	{
		std::uintptr_t fallback_state_address{};
		std::unordered_set<std::string> reported_states{};

		__declspec(noinline) void copy_state_name_safely(char* const destination,
			const std::size_t destination_size, const char* const name)
		{
			if (!name)
			{
				return;
			}

			__try
			{
				strncpy_s(destination, destination_size, name, _TRUNCATE);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				strcpy_s(destination, destination_size, "<invalid pointer>");
			}
		}

		__declspec(noinline) void copy_texture_context_safely(const std::uintptr_t owner,
			char* const material_name, const std::size_t material_name_size,
			std::uint32_t* const technique_slot, std::uint16_t* const pass_index,
			std::uintptr_t* const source)
		{
			// QoS PC sub_103819D0 passes its command buffer to sub_103817B0.
			// These offsets identify the material and pass active at the fatal bind.
			__try
			{
				const auto material = *reinterpret_cast<const std::uintptr_t*>(owner + 136);
				const auto name = material
					? *reinterpret_cast<const char* const*>(material) : nullptr;
				if (name)
				{
					strncpy_s(material_name, material_name_size, name, _TRUNCATE);
				}
				*technique_slot = *reinterpret_cast<const std::uint32_t*>(owner + 140);
				*pass_index = *reinterpret_cast<const std::uint16_t*>(owner + 152);
				*source = *reinterpret_cast<const std::uintptr_t*>(owner + 80);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				strcpy_s(material_name, material_name_size, "<unreadable>");
				*technique_slot = 0;
				*pass_index = 0;
				*source = 0;
			}
		}

		void report_invalid_state(const int path, const char* name, const std::uint32_t encoded_index,
			const std::uintptr_t owner)
		{
			char safe_name[128] = "(null)";
			copy_state_name_safely(safe_name, sizeof(safe_name), name);

			const auto state_index = encoded_index / 3;
			const auto fallback = *reinterpret_cast<const void**>(fallback_state_address);
			if (path == 0 || path == 1)
			{
				char material_name[128] = "<null>";
				std::uint32_t technique_slot = 0;
				std::uint16_t pass_index = 0;
				std::uintptr_t source = 0;
				copy_texture_context_safely(owner, material_name, sizeof(material_name),
					&technique_slot, &pass_index, &source);
				const auto key = std::to_string(path) + ":" + std::to_string(state_index)
					+ ":" + material_name + ":" + std::to_string(technique_slot)
					+ ":" + std::to_string(pass_index);
				if (!reported_states.insert(key).second)
				{
					return;
				}
				console::warn(
					"[renderer] unbound code texture slot %u (path %d): material='%s', technique=%u, pass=%u, source=0x%08X, binding='%s'; using fallback texture 0x%08X\n",
					state_index, path, material_name, technique_slot, pass_index, source, safe_name,
					reinterpret_cast<std::uintptr_t>(fallback));
				return;
			}
			if (!reported_states.insert(std::to_string(path) + ":"
				+ std::to_string(state_index) + ":" + safe_name).second)
			{
				return;
			}
			console::warn(
				"[renderer] invalid state slot %u in R_SetState path %d: name='%s', owner=0x%08X; using fallback state 0x%08X\n",
				state_index, path, safe_name, owner, reinterpret_cast<std::uintptr_t>(fallback));
		}

		__declspec(naked) void set_pass_state_fallback_stub()
		{
			__asm
			{
				pushad
				// The fifth Com_Error argument is at entry ESP+20, then pushad adds 32.
				mov eax, [esp + 0x34]
				mov ecx, [esp + 0x1C]
				mov edx, [esp + 0x08]
				push edx
				push ecx
				push eax
				push 0
				call report_invalid_state
				add esp, 0x10
				popad

				mov eax, fallback_state_address
				mov esi, [eax]
				ret
			}
		}

		__declspec(naked) void set_primitive_state_fallback_stub()
		{
			__asm
			{
				pushad
				// QoS PC 1.1, 0x10381992: the name is the fifth Com_Error
				// argument, ECX is 3 * state slot, and EBX owns the state table.
				mov eax, [esp + 0x34]
				mov ecx, [esp + 0x18]
				mov edx, [esp + 0x10]
				push edx
				push ecx
				push eax
				push 1
				call report_invalid_state
				add esp, 0x10
				popad

				mov eax, fallback_state_address
				mov ebp, [eax]
				ret
			}
		}

		__declspec(naked) void validate_state_fallback_stub()
		{
			__asm
			{
				pushad
				mov eax, [esp + 0x38]
				mov ecx, [esp + 0x1C]
				mov edx, [esp + 0x18]
				push edx
				push ecx
				push eax
				push 2
				call report_invalid_state
				add esp, 0x10
				popad
				ret
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// QoS PC 103812A0/103817B0 bind code textures through 10385910
			// (D3D SetTexture), not MaterialStateBits. Keep the existing texture
			// fallback, reporting each affected material/pass rather than only a slot.
			fallback_state_address = game::game_offset(0x10C4A2C8);
			utils::hook::call(game::game_offset(0x103813EC), set_pass_state_fallback_stub);
			utils::hook::call(game::game_offset(0x10381992), set_primitive_state_fallback_stub);
			utils::hook::call(game::game_offset(0x10384C6C), validate_state_fallback_stub);
			scheduler::on_shutdown([] { reported_states.clear(); });
		}
	};
}

REGISTER_COMPONENT(renderer_state_validation::component)
