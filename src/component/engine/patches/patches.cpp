#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/console.hpp"
#include "component/engine/console/command.hpp"
#include "component/engine/patches/xlive.hpp"
#include "component/utils/resources.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/flags.hpp>
#include <utils/string.hpp>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <d3d9.h>
#include <mmsystem.h>
#include <intrin.h>

#pragma comment(lib, "winmm.lib")
#ifndef VERSION_BUILD
#define VERSION_BUILD "0"
#endif

//#define XLIVELESS

namespace patches
{
	namespace
	{
		game::dvar_s* pm_adsStopsSprint{};
		// QoS PM_UpdateAimDownSightFlag at 1021D2AF shifts by 11.
		// The old BUTTON_ADS declaration (0x200) actually requests crouch.
		constexpr unsigned int qos_ads_button = 0x800u;
	}

	void enforce_ads_sprint_interrupt(game::usercmd_t* cmd)
	{
		if (!cmd)
		{
			return;
		}

		if (pm_adsStopsSprint && pm_adsStopsSprint->current.enabled
			&& (cmd->buttons & qos_ads_button) != 0)
		{
			cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons & ~game::BUTTON_SPRINT);
		}
	}

	namespace
	{
		utils::hook::detour update_sprint_hook;
		utils::hook::detour move_single_hook;
		std::uintptr_t end_sprint_address{};
		game::dvar_s* overhead_font_dvar{};
		std::uintptr_t overhead_font_continue{};
		game::dvar_s* pm_allowProne{};
		std::uintptr_t prone_allowed_continue{};
		std::uintptr_t prone_blocked_continue{};
		game::dvar_s* pm_adsExitOnDamage{};
		std::uintptr_t ads_damage_continue{};
		std::uintptr_t ads_normal_continue{};
		game::dvar_s* pm_allowCover{};
		game::dvar_s* pm_mantleFirstPerson{};
		game::dvar_s* pm_climbFirstPerson{};
		game::dvar_s* pm_airborneBobScale{};
		game::dvar_s* pm_movement_mode{};
		const char* movement_mode_names[] = { "stock", "iw3" };
		int applied_movement_mode = -1;

		// Only the host/frontend expands presets. Remote prediction consumes the
		// individual replicated values; it must not overwrite the host's tuning.
		void apply_movement_preset()
		{
			if (!pm_movement_mode)
				return;
			const auto* const server = game::Dvar_FindVar("sv_running");
			const auto* const ingame = game::Dvar_FindVar("cl_ingame");
			if (ingame && ingame->current.enabled && (!server || !server->current.enabled))
			{
				applied_movement_mode = -1;
				return;
			}
			const int mode = pm_movement_mode->current.integer;
			if (mode == applied_movement_mode || mode < 0 || mode > 1)
				return;
			struct setting { const char* name; const char* stock; const char* iw3; };
			constexpr setting settings[] = {
				{ "pm_adsStopsSprint", "0", "1" },
				{ "pm_adsExitOnDamage", "1", "0" },
				{ "pm_allowProne", "2", "1" },
				{ "pm_allowCover", "1", "0" },
				{ "pm_mantleFirstPerson", "0", "1" },
				{ "pm_climbFirstPerson", "0", "1" },
				{ "pm_airborneBobScale", "1", "0" },
			};
			for (const auto& setting : settings)
				game::Dvar_SetString(setting.name, mode == 0 ? setting.stock : setting.iw3);
			applied_movement_mode = mode;
			console::info("[movement] applied %s preset\n", movement_mode_names[mode]);
		}
		utils::hook::detour cover_update_hook;
		utils::hook::detour cover_permission_hook;
		utils::hook::detour cover_entry_hook;
		utils::hook::detour camera_selection_hook;
		utils::hook::detour horizontal_bob_hook;
		utils::hook::detour vertical_bob_hook;
		void* cover_update_original{};
		void* cover_permission_original{};
		void* cover_entry_original{};
		void* horizontal_bob_original{};
		void* vertical_bob_original{};
		std::uintptr_t cover_exit_address{};
		std::uintptr_t movement_tls_index_address{};
		std::uintptr_t traversal_camera_continue{};
		std::uintptr_t traversal_camera_first_person{};
		std::uintptr_t traversal_camera_entity_address{};
		std::uintptr_t model_camera_continue{};
		std::uintptr_t model_camera_end{};

		// QoS PC traversal enum is independently confirmed by the string table
		// at 10556324 and 101F0FE0's store to playerState +3956. These are NOT
		// COD4 mantleState fields: QoS owns the movement/animation simulation.
		bool first_person_traversal_requested()
		{
			const auto* const ps = reinterpret_cast<const unsigned char*>(game::game_offset(0x12A4CDFC));
			if (*reinterpret_cast<const int*>(ps + 4) >= 6
				|| (*reinterpret_cast<const unsigned int*>(ps + 12) & 8u) == 0)
				return false;
			const auto* const third_person = *reinterpret_cast<game::dvar_s**>(game::game_offset(0x113F25F8));
			if (third_person && third_person->current.enabled)
				return false; // Preserve an explicit third-person request.
			const int type = *reinterpret_cast<const int*>(ps + 3956);
			if (type >= 4 && type <= 6) // mantle_40 / mantle_44 / mantle_56
				return pm_mantleFirstPerson && pm_mantleFirstPerson->current.enabled;
			if (type == 2 || type == 3 || type == 7) // ladder / ledge / pipe
				return pm_climbFirstPerson && pm_climbFirstPerson->current.enabled;
			return false; // Balance, transitions and wallhug keep their native camera.
		}

		// Preserve all engine register/FPU state when querying from usercall sites.
		// EAX alone carries the predicate result; PUSHAD's saved EAX is +28.
		__declspec(naked) void query_first_person_traversal()
		{
			__asm
			{
				pushfd
				pushad
				mov ebp, esp
				sub esp, 528
				and esp, -16
				fxsave [esp]
				call first_person_traversal_requested
				movzx eax, al
				mov [ebp + 28], eax
				fxrstor [esp]
				mov esp, ebp
				popad
				popfd
				ret
			}
		}

		int* __cdecl camera_selection_stub()
		{
			auto* const result = camera_selection_hook.invoke<int*>();
			const auto* const snapshot = *reinterpret_cast<const unsigned char**>(game::game_offset(0x129FE8E4));
			if (snapshot && *reinterpret_cast<const int*>(snapshot + 16) < 11
				&& first_person_traversal_requested())
				*reinterpret_cast<int*>(game::game_offset(0x12A4CDE8)) = 0;
			return result;
		}

		__declspec(naked) void traversal_camera_stub()
		{
			__asm
			{
				push eax
				call query_first_person_traversal
				test eax, eax
				pop eax
				jnz first_person
				push eax
				mov eax, dword ptr[traversal_camera_entity_address]
				cmp dword ptr[eax + edx], 3FFh
				pop eax
				jmp dword ptr[traversal_camera_continue] // Native JZ consumes this CMP.
			first_person:
				jmp dword ptr[traversal_camera_first_person]
			}
		}

		__declspec(naked) void model_camera_stub()
		{
			__asm
			{
				push eax
				call query_first_person_traversal
				test eax, eax
				pop eax
				jnz finished
				test edi, edi
				jz finished
				jmp dword ptr[model_camera_continue]
			finished:
				jmp dword ptr[model_camera_end]
			}
		}

		// Disable new cover entry without bypassing QoS's native permission rules.
		__declspec(naked) void cover_permission_stub()
		{
			__asm
			{
				push eax
				mov eax, dword ptr[pm_allowCover]
				test eax, eax
				jz native_permission
				cmp byte ptr[eax + 10h], 0
				jne native_permission
				pop eax
				and dword ptr[esi + 10h], 0FFFFBFFFh // Clear the native entry hint.
				xor eax, eax
				ret
			native_permission:
				pop eax
				jmp dword ptr[cover_permission_original]
			}
		}

		void __cdecl leave_disabled_cover(unsigned char* pm)
		{
			if (!pm_allowCover || pm_allowCover->current.enabled)
				return;
			auto* const ps = *reinterpret_cast<unsigned char**>(pm);
			const auto cover_flags = *reinterpret_cast<unsigned int*>(ps + 4032);
			if ((cover_flags & 1u) == 0 || (cover_flags & 0x10u) != 0)
				return;
			// Same BG per-thread context lookup as 101FF5C0. Begin the native
			// forced exit (101FF410), then let the original update finish it.
			const auto tls_index = *reinterpret_cast<unsigned int*>(movement_tls_index_address);
			const auto* const tls_slots = reinterpret_cast<std::uintptr_t*>(__readfsdword(0x2C));
			const auto bg = *reinterpret_cast<unsigned char**>(tls_slots[tls_index] + 88);
			const auto context_for_pm = *reinterpret_cast<int(__cdecl**)(unsigned char*)>(bg + 160528);
			const int context = context_for_pm(pm);
			__asm
			{
				mov esi, ps
				push 0
				push 1
				push context
				call dword ptr[cover_exit_address]
				add esp, 12
			}
		}

		__declspec(naked) void cover_entry_stub()
		{
			// Central entry (101FD3D0), including auto-cover and queued requests.
			// Native ABI is EAX = trace/origin input, stack +4 = pmove.
			// Keep EAX for the native path; the no-entry result is the pmove pointer.
			__asm
			{
				push edx
				mov edx, dword ptr[pm_allowCover]
				test edx, edx
				jz native_entry
				cmp byte ptr[edx + 10h], 0
				jne native_entry
				pop edx
				mov eax, [esp + 4]
				ret
			native_entry:
				pop edx
				jmp dword ptr[cover_entry_original]
			}
		}

		__declspec(naked) void cover_update_stub()
		{
			__asm
			{
				pushfd
				pushad
				mov ebp, esp
				sub esp, 528
				and esp, -16
				fxsave [esp]
				push edi // QoS PM_UpdateCover takes pmove in EDI.
				call leave_disabled_cover
				add esp, 4
				fxrstor [esp]
				mov esp, ebp
				popad
				popfd
				jmp dword ptr[cover_update_original]
			}
		}

		// Native camera bob helpers: ECX = playerState; stack = cycle, speed,
		// max amplitude. Scale only their copied speed argument, not velocity,
		// player state, the shared bg_bobMax dvar, damage kick or recoil.
		__declspec(naked) void horizontal_bob_stub()
		{
			__asm
			{
				push eax
				mov eax, dword ptr[pm_airborneBobScale]
				test eax, eax
				jz finished
				cmp dword ptr[ecx + 80h], 3FFh
				jne finished
				test byte ptr[ecx + 0Ch], 8
				jnz finished
				fld dword ptr[esp + 0Ch]
				fmul dword ptr[eax + 10h]
				fstp dword ptr[esp + 0Ch]
			finished:
				pop eax
				jmp dword ptr[horizontal_bob_original]
			}
		}

		__declspec(naked) void vertical_bob_stub()
		{
			__asm
			{
				push eax
				mov eax, dword ptr[pm_airborneBobScale]
				test eax, eax
				jz finished
				cmp dword ptr[ecx + 80h], 3FFh
				jne finished
				test byte ptr[ecx + 0Ch], 8
				jnz finished
				fld dword ptr[esp + 0Ch]
				fmul dword ptr[eax + 10h]
				fstp dword ptr[esp + 0Ch]
			finished:
				pop eax
				jmp dword ptr[vertical_bob_original]
			}
		}

		__declspec(naked) void ads_damage_stub()
		{
			__asm
			{
				push edx
				mov edx, dword ptr[pm_adsExitOnDamage]
				test edx, edx
				jz native_damage
				cmp byte ptr[edx + 10h], 0
				je keep_ads
			native_damage:
				pop edx
				cmp al, bl
				je normal_ads
				cmp dword ptr[esi + 15Ch], ebx
				jmp dword ptr[ads_damage_continue]
			keep_ads:
				pop edx
			normal_ads:
				jmp dword ptr[ads_normal_continue]
			}
		}

		// PlayerProneAllowed retains native ground/clearance traces. The dvar
		// overrides only the weapon no-prone gate, never the geometry result.
		__declspec(naked) void prone_permission_stub()
		{
			__asm
			{
				push edx
				mov edx, dword ptr[pm_allowProne]
				test edx, edx
				jz native_gate
				cmp dword ptr[edx + 10h], 2
				je native_gate // Preset stock must restore the weapon's own gate.
				cmp dword ptr[edx + 10h], 0
				pop edx
				je blocked
				jmp dword ptr[prone_allowed_continue]
			native_gate:
				pop edx
				cmp dword ptr[edx + 5ACh], 0
				jne blocked
				jmp dword ptr[prone_allowed_continue]
			blocked:
				jmp dword ptr[prone_blocked_continue]
			}
		}

		// QoS PC 1.1 PM_EndSprint: EAX = playerState, ECX = pmove.
		// Retain its timer, button-release and per-client TLS side effects.
		void end_sprint(void* ps, void* pm)
		{
			__asm
			{
				mov eax, ps
				mov ecx, pm
				call dword ptr[end_sprint_address]
			}
		}

		void __cdecl update_sprint_stub(unsigned char* pm)
		{
			// Shared prediction/server path, not a keyboard or gamepad adapter.
			// Layout checked against 101E4D70 and 101DC6C0, not imported from COD4.
			auto* const ps = *reinterpret_cast<unsigned char**>(pm);
			auto& buttons = *reinterpret_cast<unsigned int*>(pm + 8);
			const bool interrupt = pm_adsStopsSprint && pm_adsStopsSprint->current.enabled
				&& (buttons & qos_ads_button) != 0;
			if (interrupt)
			{
				if ((*reinterpret_cast<unsigned int*>(ps + 12) & 0x8000u) != 0)
					end_sprint(ps, pm);
				buttons &= ~static_cast<unsigned int>(game::BUTTON_SPRINT);
			}
			update_sprint_hook.invoke<void>(pm);
			// Leave sprint suppressed through the later ADS/weapon update.
			// The outer movement wrapper restores the command after this step.
		}

		int __cdecl move_single_stub(unsigned char* pm, int argument)
		{
			auto& buttons = *reinterpret_cast<unsigned int*>(pm + 8);
			const auto sprint = buttons & static_cast<unsigned int>(game::BUTTON_SPRINT);
			const bool interrupt = pm_adsStopsSprint && pm_adsStopsSprint->current.enabled
				&& (buttons & qos_ads_button) != 0;
			const auto result = move_single_hook.invoke<int>(pm, argument);
			if (interrupt)
				buttons = (buttons & ~static_cast<unsigned int>(game::BUTTON_SPRINT)) | sprint;
			return result;
		}

		game::Font_s* select_overhead_font()
		{
			auto* const native_font = *reinterpret_cast<game::Font_s**>(game::game_offset(0x113FB174));
			constexpr const char* names[] = {nullptr, "fonts/normalFont", "fonts/bigfont", "fonts/smallfont", "fonts/boldfont"};
			const int index = overhead_font_dvar ? overhead_font_dvar->current.integer : 0;
			if (index <= 0 || index >= static_cast<int>(std::size(names)))
				return native_font;
			// Do not cache zone-owned pointers across map changes or request a missing asset.
			if (game::DB_IsXAssetDefault(game::ASSET_TYPE_FONT, names[index]))
				return native_font;
			auto* const font = game::R_RegisterFont(names[index]);
			return font && font->pixelHeight > 0 ? font : native_font;
		}

		// Replace only the font load. Native name/team/occlusion/projection and
		// normalized cg_overheadNamesSize scaling continue unchanged.
		__declspec(naked) void overhead_font_stub()
		{
			__asm
			{
				pushfd
				pushad
				sub esp, 128
				movdqu [esp], xmm0
				movdqu [esp + 16], xmm1
				movdqu [esp + 32], xmm2
				movdqu [esp + 48], xmm3
				movdqu [esp + 64], xmm4
				movdqu [esp + 80], xmm5
				movdqu [esp + 96], xmm6
				movdqu [esp + 112], xmm7
				call select_overhead_font
				mov [esp + 136], eax // saved EBP in PUSHAD
				movdqu xmm0, [esp]
				movdqu xmm1, [esp + 16]
				movdqu xmm2, [esp + 32]
				movdqu xmm3, [esp + 48]
				movdqu xmm4, [esp + 64]
				movdqu xmm5, [esp + 80]
				movdqu xmm6, [esp + 96]
				movdqu xmm7, [esp + 112]
				add esp, 128
				popad
				popfd
				jmp dword ptr[overhead_font_continue]
			}
		}

		void apply_input_and_overhead_patches()
		{
			const auto cover_site = game::game_offset(0x1020DC30);
			const auto permission_site = game::game_offset(0x101FF670);
			const auto entry_site = game::game_offset(0x101FD3D0);
			constexpr unsigned char entry_bytes[] = {0x83, 0xEC, 0x10, 0x53, 0x55, 0x56};
			// Both prologues load a relocated dvar pointer, then CMP enabled,0.
			const auto matches_bool_load = [](std::uintptr_t site, std::uintptr_t global)
			{
				constexpr unsigned char suffix[] = {0x80, 0x78, 0x10, 0x00};
				return *reinterpret_cast<unsigned char*>(site) == 0xA1
					&& *reinterpret_cast<std::uintptr_t*>(site + 1) == game::game_offset(global)
					&& std::memcmp(reinterpret_cast<void*>(site + 5), suffix, sizeof(suffix)) == 0;
			};
			if (matches_bool_load(cover_site, 0x118EC9E8)
				&& matches_bool_load(permission_site, 0x118EC824)
				&& std::memcmp(reinterpret_cast<void*>(entry_site), entry_bytes, sizeof(entry_bytes)) == 0)
			{
				cover_exit_address = game::game_offset(0x101FF410);
				movement_tls_index_address = game::game_offset(0x105805DC);
				cover_update_hook.create(cover_site, cover_update_stub);
				cover_update_original = cover_update_hook.get_original();
				cover_permission_hook.create(permission_site, cover_permission_stub);
				cover_permission_original = cover_permission_hook.get_original();
				cover_entry_hook.create(entry_site, cover_entry_stub);
				cover_entry_original = cover_entry_hook.get_original();
			}
			else console::warn("[movement] cover toggle skipped: unexpected engine instructions\n");

			const auto camera_site = game::game_offset(0x102A3EC8);
			const auto model_site = game::game_offset(0x102A364A);
			constexpr unsigned char camera_cmp[] = {0xFF, 0x03, 0x00, 0x00};
			constexpr unsigned char model_bytes[] = {0x85, 0xFF, 0x0F, 0x84, 0x20, 0x03, 0x00, 0x00};
			const auto selection_site = game::game_offset(0x102A0320);
			if (matches_bool_load(selection_site, 0x113F25F8)
				&& *reinterpret_cast<unsigned short*>(camera_site) == 0xBA81
				&& *reinterpret_cast<std::uintptr_t*>(camera_site + 2) == game::game_offset(0x12A7F124)
				&& std::memcmp(reinterpret_cast<void*>(camera_site + 6), camera_cmp, sizeof(camera_cmp)) == 0
				&& std::memcmp(reinterpret_cast<void*>(model_site), model_bytes, sizeof(model_bytes)) == 0)
			{
				// Use the native first-person view branch. Suppress only traversal's
				// external/model camera; never clear replicated movement flags.
				traversal_camera_entity_address = game::game_offset(0x12A7F124);
				traversal_camera_continue = camera_site + 10;
				traversal_camera_first_person = game::game_offset(0x102A3EE0);
				model_camera_continue = model_site + sizeof(model_bytes);
				model_camera_end = game::game_offset(0x102A3972);
				camera_selection_hook.create(selection_site, camera_selection_stub);
				utils::hook::nop(camera_site, 10);
				utils::hook::jump(camera_site, traversal_camera_stub);
				utils::hook::nop(model_site, sizeof(model_bytes));
				utils::hook::jump(model_site, model_camera_stub);
			}
			else console::warn("[movement] first-person traversal skipped: unexpected engine instructions\n");

			constexpr unsigned char bob_bytes[] = {0x8B, 0x81, 0x28, 0x01, 0x00, 0x00};
			const auto horizontal_site = game::game_offset(0x102A05D0);
			const auto vertical_site = game::game_offset(0x102A06C0);
			if (std::memcmp(reinterpret_cast<void*>(horizontal_site), bob_bytes, sizeof(bob_bytes)) == 0
				&& std::memcmp(reinterpret_cast<void*>(vertical_site), bob_bytes, sizeof(bob_bytes)) == 0)
			{
				horizontal_bob_hook.create(horizontal_site, horizontal_bob_stub);
				horizontal_bob_original = horizontal_bob_hook.get_original();
				vertical_bob_hook.create(vertical_site, vertical_bob_stub);
				vertical_bob_original = vertical_bob_hook.get_original();
			}
			else console::warn("[movement] airborne bob control skipped: unexpected engine instructions\n");

			const auto damage_site = game::game_offset(0x1021E925);
			constexpr unsigned char damage_bytes[] = {0x3A, 0xC3, 0x74, 0x2D, 0x39, 0x9E, 0x5C, 0x01, 0x00, 0x00};
			if (std::memcmp(reinterpret_cast<void*>(damage_site), damage_bytes, sizeof(damage_bytes)) == 0)
			{
				// COD4 PM_UpdateAimDownSightLerp comparison, validated in QoS.
				// QoS also has a per-player override; gate the branch itself so
				// that override cannot re-enable cancellation when the server opts out.
				ads_damage_continue = game::game_offset(0x1021E92F);
				ads_normal_continue = game::game_offset(0x1021E956);
				utils::hook::nop(damage_site, sizeof(damage_bytes));
				utils::hook::jump(damage_site, ads_damage_stub);
			}
			else console::warn("[movement] ADS damage patch skipped: unexpected engine instructions\n");
			const auto prone_site = game::game_offset(0x101DF520);
			constexpr unsigned char prone_bytes[] = {0x83, 0xBA, 0xAC, 0x05, 0x00, 0x00, 0x00, 0x75, 0x73};
			if (std::memcmp(reinterpret_cast<void*>(prone_site), prone_bytes, sizeof(prone_bytes)) == 0)
			{
				prone_allowed_continue = game::game_offset(0x101DF529);
				prone_blocked_continue = game::game_offset(0x101DF59C);
				utils::hook::nop(prone_site, sizeof(prone_bytes));
				utils::hook::jump(prone_site, prone_permission_stub);
			}
			else console::warn("[movement] prone patch skipped: unexpected engine instructions\n");
			const auto bind_site = game::game_offset(0x10319F0A);
			constexpr unsigned char bind_bytes[] = {0x8D, 0x44, 0x24, 0x10, 0x8D, 0x50, 0x01};
			if (std::memcmp(reinterpret_cast<void*>(bind_site), bind_bytes, sizeof(bind_bytes)) == 0)
			{
				// KisakCOD Key_Bind_f accepts arbitrary commands. QoS additionally
				// lowercases and whitelists 36 actions here; skip only that restriction.
				// Keep native key parsing, quoting, allocation and archive dirty flag.
				utils::hook::nop(bind_site, sizeof(bind_bytes));
				utils::hook::jump(bind_site, game::game_offset(0x10319F9C));
			}
			else console::warn("[input] bind patch skipped: unexpected engine instructions\n");

			const auto sprint_site = game::game_offset(0x101E4D70);
			constexpr unsigned char sprint_bytes[] = {0x51, 0xA1, 0x98, 0xBC, 0x6C, 0x10};
			// Absolute operands are rebased by Windows: compare the opcode and
			// resolved dvar address rather than the IDA preferred-base bytes.
			constexpr unsigned char move_bytes[] = {0x81, 0xEC, 0x9C, 0x00, 0x00, 0x00};
			const auto move_site = game::game_offset(0x101E54C0);
			if (std::memcmp(reinterpret_cast<void*>(move_site), move_bytes, sizeof(move_bytes)) == 0
				&& *reinterpret_cast<unsigned char*>(sprint_site) == sprint_bytes[0]
				&& *reinterpret_cast<unsigned char*>(sprint_site + 1) == sprint_bytes[1]
				&& *reinterpret_cast<std::uintptr_t*>(sprint_site + 2) == game::game_offset(0x106CBC98))
			{
				end_sprint_address = game::game_offset(0x101DC6C0);
				update_sprint_hook.create(sprint_site, update_sprint_stub);
				move_single_hook.create(move_site, move_single_stub);
			}
			else console::warn("[movement] sprint ADS patch skipped: unexpected engine instructions\n");

			const auto font_site = game::game_offset(0x10284FF8);
			if (*reinterpret_cast<unsigned short*>(font_site) == 0x2D8B
				&& *reinterpret_cast<std::uintptr_t*>(font_site + 2) == game::game_offset(0x113FB174))
			{
				overhead_font_continue = font_site + 6;
				utils::hook::nop(font_site, 6);
				utils::hook::jump(font_site, overhead_font_stub);
			}
			else console::warn("[renderer] overhead font patch skipped: unexpected engine instructions\n");
		}

		std::uintptr_t noclip_cmd_scale_address{};
		std::uintptr_t noclip_wish_continue{};
		game::dvar_s* pm_noclipScale{};

		// QoS PC 1.1: EDI is pmove_t; cmd.buttons is at +8. Jump_Check
		// tests 0x400, PM_CheckDuck tests 0x100. Both pressed cancel out.
		__declspec(naked) void noclip_vertical_input()
		{
			__asm
			{
				push ecx
				mov ecx, [edi + 8]
				xor eax, eax
				test ecx, 400h
				jz check_crouch
				mov eax, 127
			check_crouch:
				test ecx, 100h
				jz finished
				sub eax, 127
			finished:
				pop ecx
				ret
			}
		}

		__declspec(naked) void noclip_cmd_scale_stub()
		{
			__asm
			{
				push eax
				call noclip_vertical_input
				cvtsi2ss xmm0, eax
				// Replace the native zero-valued third PM_CmdScale argument.
				movss dword ptr[esp + 8], xmm0
				pop eax
				// Forward the stack argument through the extra call frame.
				push dword ptr[esp + 4]
				call dword ptr[noclip_cmd_scale_address]
				add esp, 4
				push ecx
				mov ecx, dword ptr[pm_noclipScale]
				test ecx, ecx
				jz finished
				// PM_CmdScale returns the native speed scale in XMM0.
				mulss xmm0, dword ptr[ecx + 10h]
			finished:
				pop ecx
				ret
			}
		}

		__declspec(naked) void noclip_wish_velocity_stub()
		{
			__asm
			{
				push eax
				call noclip_vertical_input
				cvtsi2ss xmm4, eax
				cvtps2pd xmm4, xmm4
				pop eax
				// Replay the displaced addition; native code then adds view-up
				// times XMM4 to each component, as in the COD4 noclip path.
				addsd xmm2, xmm3
				jmp dword ptr[noclip_wish_continue]
			}
		}

		void apply_noclip_vertical_input()
		{
			const auto scale_call = game::game_offset(0x101E174D);
			const auto wish_zero = game::game_offset(0x101E1781);
			constexpr unsigned char scale_bytes[] = {0xE8, 0x7E, 0xC2, 0xFF, 0xFF};
			constexpr unsigned char wish_bytes[] = {0x0F, 0x57, 0xE4, 0xF2, 0x0F, 0x58, 0xD3};
			if (std::memcmp(reinterpret_cast<const void*>(scale_call), scale_bytes, sizeof(scale_bytes)) != 0
				|| std::memcmp(reinterpret_cast<const void*>(wish_zero), wish_bytes, sizeof(wish_bytes)) != 0)
			{
				console::warn("[movement] noclip vertical patch skipped: unexpected engine instructions\n");
				return;
			}

			// Only PM_NOCLIP (pm_type 2) reaches these sites. Keep native
			// friction, speed normalization, acceleration and shared prediction.
			noclip_cmd_scale_address = game::game_offset(0x101DD9D0);
			noclip_wish_continue = game::game_offset(0x101E1788);
			utils::hook::call(scale_call, noclip_cmd_scale_stub);
			utils::hook::nop(wish_zero, sizeof(wish_bytes));
			utils::hook::jump(wish_zero, noclip_wish_velocity_stub);
		}

		constexpr std::size_t k_huffman_max_decoded_bytes = 0x20000;
		constexpr std::size_t k_huffman_max_compressed_bytes = k_huffman_max_decoded_bytes;
		constexpr std::size_t k_ui_replace_directive_max_len = 0x100;
		constexpr std::size_t k_party_member_join_max_message_bytes = 0x4000;
		const char* cg_draw_fps_modes[] =
		{
			"Off",
			"Simple",
			"Detailed",
			"Detailed 3",
			"Detailed 4",
			"Detailed 5",
			"Detailed 6",
			"Detailed 7",
		};
		std::uintptr_t cg_draw_fps_dvar_pointer_address{};
		std::uintptr_t cg_draw_fps_detail_continue{};
		std::uintptr_t cg_draw_fps_simple_exit{};

		__declspec(naked) void cg_draw_fps_detail_gate_stub()
		{
			__asm
			{
				// Replay QoS PC 1.1 0x102B4FFA-0x102B500E, then let mode 1
				// leave after the FPS line while modes 2+ draw the base detail block.
				movss xmm1, dword ptr[ebp + 8]
				cvtss2sd xmm0, xmm0
				cvtps2pd xmm1, xmm1
				addsd xmm0, xmm1
				cvtsd2ss xmm0, xmm0
				movss dword ptr[ebp + 8], xmm0
				mov ecx, dword ptr[cg_draw_fps_dvar_pointer_address]
				mov ecx, dword ptr[ecx]
				cmp dword ptr[ecx + 10h], 1
				je simple
				jmp dword ptr[cg_draw_fps_detail_continue]

			simple:
				// The skipped native block consumes all five arguments from the
				// preceding draw call before reaching the shared /GS epilogue.
				add esp, 14h
				jmp dword ptr[cg_draw_fps_simple_exit]
			}
		}

		void apply_cg_draw_fps_modes()
		{
			cg_draw_fps_dvar_pointer_address = game::game_offset(0x113F25F0);
			cg_draw_fps_detail_continue = game::game_offset(0x102B5013);
			cg_draw_fps_simple_exit = game::game_offset(0x102B54BF);

			utils::hook::jump(game::game_offset(0x102B4FFA), cg_draw_fps_detail_gate_stub);
			utils::hook::nop(game::game_offset(0x102B51BA), 6);
			utils::hook::jump(game::game_offset(0x102B51BA), cg_draw_fps_simple_exit);
		}

		std::size_t bounded_length(const char* value, const std::size_t max_len)
		{
			if (!value)
			{
				return 0;
			}

			std::size_t length = 0;
			while (length < max_len && value[length] != '\0')
			{
				++length;
			}

			return length;
		}

		std::string build_shortversion_string()
		{
			std::string version = VERSION_PRODUCT;

#ifdef DEBUG
			version += "-dbg";
#elif defined(NDEBUG)
			// release keeps the plain semantic version
#else
			version += "-nightly";
#endif

			return version;
		}

		std::string build_build_label()
		{
			std::string short_hash = GIT_HASH;
			if (short_hash.size() > 7)
			{
				short_hash.resize(7);
			}

			if (GIT_DIRTY)
			{
				short_hash += "-dirty";
			}

			return short_hash;
		}

		std::string build_timestamp_label()
		{
			return std::string(__DATE__) + " " + __TIME__;
		}

		std::string build_game_date_string()
		{
			return __DATE__;
		}

		std::string build_version_string()
		{
			return "Project: Consolation "
				+ build_shortversion_string()
				+ " build "
				+ build_build_label()
				+ " "
				+ build_timestamp_label()
				+ " win-x86";
		}

		int ret_one(DWORD*, int)
		{
			return 1;
		}

		void jump_to_generated_stub(const std::uintptr_t site, void* stub)
		{
			if (!stub)
			{
				throw std::runtime_error("Generated jump stub is null");
			}
			if (!utils::hook::is_relatively_far(reinterpret_cast<const void*>(site), stub))
			{
				utils::hook::jump(site, stub);
				return;
			}

			// QoS PC 1.1 is x86. A nearby push/ret bridge reaches a JIT stub
			// regardless of where AsmJit allocated it, without altering registers.
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const auto granularity = static_cast<std::uint64_t>(info.dwAllocationGranularity);
			const auto maximum = std::min<std::uint64_t>(
				reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress),
				static_cast<std::uint64_t>(site) + std::numeric_limits<std::int32_t>::max());
			for (std::uint64_t address = site; address < maximum;)
			{
				MEMORY_BASIC_INFORMATION region{};
				if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
					&region, sizeof(region)))
				{
					break;
				}
				const auto region_end = static_cast<std::uint64_t>(
					reinterpret_cast<std::uintptr_t>(region.BaseAddress)) + region.RegionSize;
				if (region.State == MEM_FREE)
				{
					const auto aligned = (address + granularity - 1) & ~(granularity - 1);
					if (aligned < region_end && region_end - aligned >= 6 && aligned < maximum)
					{
					auto* bridge = static_cast<unsigned char*>(VirtualAlloc(
						reinterpret_cast<void*>(static_cast<std::uintptr_t>(aligned)), 6,
						MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
						if (bridge)
						{
						bridge[0] = 0x68; // push imm32
						const auto destination = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stub));
						std::memcpy(bridge + 1, &destination, sizeof(destination));
						bridge[5] = 0xC3; // ret
						DWORD old_protect{};
						if (!VirtualProtect(bridge, 6, PAGE_EXECUTE_READ, &old_protect))
						{
							VirtualFree(bridge, 0, MEM_RELEASE);
							throw std::runtime_error("Unable to protect generated jump bridge");
						}
						FlushInstructionCache(GetCurrentProcess(), bridge, 6);
						utils::hook::jump(site, bridge);
						return;
					}
					}
				}
				address = std::max(address + granularity, region_end);
			}
			throw std::runtime_error("No executable jump bridge available within rel32 range");
		}

		void apply_cinematic_stats_guard()
		{
			// QoS 1.1: cinematic command 0x104547F0 sets state 1 via 0x10454770.
			// StatsReadComplete (0x10240C60) runs stats_init.cfg for a missing profile.
			// Allow that non-network state without changing the connection state or
			// bypassing the native stat writes. Remove when the stat command is replaced.
			const auto site = game::game_offset(0x10240FF2);
			const auto state_address = static_cast<std::uint32_t>(game::game_offset(0x111F45F8));
			std::array<unsigned char, 15> expected{
				0x8B, 0x15, 0, 0, 0, 0, // mov edx, dword ptr [clcState]
				0x85, 0xD2,             // test edx, edx
				0x7E, 0x30,             // jle native_write
				0x83, 0xFA, 0x06,       // cmp edx, 6
				0x74, 0x2B,             // je native_write
			};
			std::memcpy(expected.data() + 2, &state_address, sizeof(state_address));
			if (std::memcmp(reinterpret_cast<const void*>(site), expected.data(), expected.size()) != 0)
			{
				console::error("[patches - stats] skipped: unsupported connection-state block\n");
				return;
			}

			std::array<unsigned char, 15> patch{
				0x8B, 0x15, 0, 0, 0, 0, // mov edx, dword ptr [clcState]
				0x4A,                   // dec edx (states 0 and 1 become <= 0)
				0x7E, 0x31,             // jle native_write
				0x83, 0xFA, 0x05,       // cmp edx, 5 (original state 6)
				0x74, 0x2C,             // je native_write
				0x90,
			};
			std::memcpy(patch.data() + 2, &state_address, sizeof(state_address));
			utils::hook::set(site, patch);
			console::info("[patches - stats] PATCHED: profile initialization during cinematics\n");
		}

		void disable_startup_remote_screen_sync()
		{
			// QoS 1.1 brackets two blocking startup operations with its remote-screen
			// renderer handshake: profile parsing and final renderer initialization.
			// On the supported PC build either end call can wait forever at 0x103BF658
			// when the backend misses its second acknowledgement. Neither operation
			// depends on the presentation-only handshake, so remove each balanced pair.
			struct call_patch
			{
				std::uintptr_t address;
				std::array<unsigned char, 5> expected;
			};

			constexpr std::array patches{
				call_patch{0x10243D3E, {0xE8, 0xBD, 0xC1, 0x17, 0x00}},
				call_patch{0x10243DAE, {0xE8, 0x0D, 0xB8, 0x17, 0x00}},
				call_patch{0x103FA22E, {0xE8, 0xCD, 0x5C, 0xFC, 0xFF}},
				call_patch{0x103FA243, {0xE8, 0x78, 0x53, 0xFC, 0xFF}},
			};

			for (const auto& patch : patches)
			{
				const auto site = game::game_offset(patch.address);
				if (std::memcmp(reinterpret_cast<const void*>(site), patch.expected.data(), patch.expected.size()) != 0)
				{
					console::error("[patches - startup] skipped: unsupported remote-screen call at 0x%08X\n",
						static_cast<unsigned int>(patch.address));
					return;
				}
			}

			for (const auto& patch : patches)
			{
				utils::hook::nop(game::game_offset(patch.address), patch.expected.size());
			}
			console::info("[patches - startup] PATCHED: disabled blocking startup render synchronization\n");
		}

		void apply_missing_voice_engine_guard()
		{
			// QoS 1.1 faults at 0x102462F0 when XHVCreateEngine leaves a null engine.
			// The SDK vtable and COD4A Voice_EnableMic confirm this is local-talker
			// registration. Use QoS' false-return path without setting its mic flag.
			// Retire with a replacement voice lifecycle; verify LIVE entry with and
			// without an engine, preserving normal registration when one is present.
			const auto site = game::game_offset(0x102462ED);
			const unsigned char expected[] = {0x8B, 0x47, 0x60, 0x8B, 0x08};
			const auto failure = game::game_offset(0x10246320);
			const unsigned char expected_failure[] = {0x32, 0xC0, 0x5F, 0xC3};
			if (memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0
				|| memcmp(reinterpret_cast<const void*>(failure), expected_failure, sizeof(expected_failure)) != 0)
			{
				const auto* const actual = reinterpret_cast<const unsigned char*>(site);
				console::error("[patches - voice] skipped: unsupported registration bytes at 0x102462ED "
					"(%02X %02X %02X %02X %02X)\n",
					actual[0], actual[1], actual[2], actual[3], actual[4]);
				return;
			}

			try
			{
				auto* stub = utils::hook::assemble([failure](utils::hook::assembler& a)
				{
					const auto engine_available = a.newLabel();
					a.mov(eax, dword_ptr(edi, 0x60));
					a.test(eax, eax);
					a.jnz(engine_available);
					a.jmp(reinterpret_cast<void*>(failure));
					a.bind(engine_available);
					a.mov(ecx, dword_ptr(eax));
					a.jmp(reinterpret_cast<void*>(game::game_offset(0x102462F2)));
				});
				jump_to_generated_stub(site, stub);

			// These QoS 1.1 sites load the engine and its vtable before a voice call.
			// Keep the native peer bookkeeping even without audio: COD4A's remote
			// registration has the same separation between voice and connectivity.
			// No local mic flag is set by the guard above; shutdown already checks
			// for null. Cover remote cleanup, status queries and incoming audio too.
			const auto guard_voice_call = [](const std::uintptr_t address,
				const unsigned char engine_operand, const unsigned char vtable_operand,
				const asmjit::x86::Gp& session, const asmjit::x86::Gp& vtable,
				const std::uintptr_t unavailable, const int stack_cleanup = 0)
			{
				const auto call_site = game::game_offset(address);
				const unsigned char instructions[] = {0x8B, engine_operand, 0x60, 0x8B, vtable_operand};
				if (memcmp(reinterpret_cast<const void*>(call_site), instructions, sizeof(instructions)) != 0)
				{
					console::error("[patches - voice] skipped: unsupported call-site bytes at 0x%08X\n",
						static_cast<unsigned int>(address));
					return;
				}
				auto* call_stub = utils::hook::assemble([=](utils::hook::assembler& a)
				{
					const auto available = a.newLabel();
					a.mov(eax, dword_ptr(session, 0x60));
					a.test(eax, eax);
					a.jnz(available);
					if (stack_cleanup)
					{
						a.add(esp, stack_cleanup);
					}
					if (unavailable)
					{
						a.jmp(reinterpret_cast<void*>(game::game_offset(unavailable)));
					}
					else
					{
						// Engine is null, so EAX already represents false.
						a.ret();
					}
					a.bind(available);
					a.mov(vtable, dword_ptr(eax));
					a.jmp(reinterpret_cast<void*>(call_site + sizeof(instructions)));
				});
				jump_to_generated_stub(call_site, call_stub);
			};

			// Registration still has five Com_Printf arguments to discard here.
			guard_voice_call(0x102464B4, 0x47, 0x08, edi, ecx, 0x102464F7, 0x14);
			guard_voice_call(0x102461BE, 0x46, 0x08, esi, ecx, 0x1024624A);
			guard_voice_call(0x10245BF0, 0x40, 0x08, eax, ecx, 0); // headset present
			guard_voice_call(0x10245E83, 0x40, 0x10, eax, edx, 0); // local talking
			guard_voice_call(0x10245ECF, 0x47, 0x10, edi, edx, 0x10245EB3);
			guard_voice_call(0x10245EE7, 0x47, 0x08, edi, ecx, 0x10245EB3);
			guard_voice_call(0x10245F3C, 0x47, 0x08, edi, ecx, 0x10245F28);
			guard_voice_call(0x10246100, 0x43, 0x10, ebx, edx, 0x10246139);
			// Inlined IsHeadsetPresent/IsLocalTalking calls bypass the shared helpers.
			// Resume at their result checks with EAX == 0, retaining party state and
			// native headset-change notifications instead of skipping party setup.
			guard_voice_call(0x103072A9, 0x40, 0x08, eax, ecx, 0x103072B5);
			guard_voice_call(0x10309C62, 0x40, 0x08, eax, ecx, 0x10309C6E);
			guard_voice_call(0x10309C7D, 0x40, 0x08, eax, ecx, 0x10309C89);
			guard_voice_call(0x10324F37, 0x40, 0x10, eax, edx, 0x10324F43);
			guard_voice_call(0x10324F62, 0x40, 0x10, eax, edx, 0x10324F6E);
			guard_voice_call(0x102DBE40, 0x40, 0x10, eax, edx, 0x102DBE4C);
			guard_voice_call(0x103009FD, 0x47, 0x10, edi, edx, 0x10300A09);
				console::info("[patches - voice] PATCHED: unavailable-engine lifecycle and party/UI guards\n");
			}
			catch (const std::exception& error)
			{
				console::error("[patches - voice] skipped: %s\n", error.what());
			}
		}

		void private_match_set_unpaused()
		{
			game::Dvar_SetString("cl_paused", "0");
		}

		void apply_private_match_unpause()
		{
			// COD4 clears cl_paused during SV_SpawnServer. QoS 1.1 reaches the
			// map and Game Initialization without that reset, leaving local private
			// matches on a black paused screen after ui_mp is unloaded.
			console::info("[patches - private-match] applying server startup guard\n");
			const auto site = game::game_offset(0x102F7281);
			const unsigned char expected[] = {0xFF, 0x15};
			constexpr std::size_t instruction_size = 6;
			if (memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0)
			{
				const auto* const actual = reinterpret_cast<const unsigned char*>(site);
				console::error("[patches - private-match] skipped: unexpected bytes at 0x102F7281 "
					"(%02X %02X %02X %02X %02X %02X)\n",
					actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
				return;
			}

			try
			{
				// FF 15 encodes the address of the import slot, not the slot itself.
				// Capture it before replacing the instruction with a jump.
				std::uint32_t import_slot = 0;
				memcpy(&import_slot, reinterpret_cast<const void*>(site + 2), sizeof(import_slot));
				auto* stub = utils::hook::assemble([site, import_slot](utils::hook::assembler& a)
				{
					// Preserve the original six-byte indirect import call and its
					// existing stdcall stack argument exactly.
					a.call(dword_ptr(import_slot));
					a.call(private_match_set_unpaused);
					a.jmp(reinterpret_cast<void*>(site + 6));
				});
				jump_to_generated_stub(site, stub);
				utils::hook::nop(site + 5, instruction_size - 5);
				console::info("[patches - private-match] PATCHED: clear cl_paused after server startup\n");
			}
			catch (const std::exception& error)
			{
				console::error("[patches - private-match] skipped: %s\n", error.what());
			}
		}

		bool local_offline_mode_requested()
		{
			return utils::flags::has_flag("offline")
				|| utils::flags::has_flag("local_offline")
				|| utils::flags::has_flag("local-offline")
				|| xlive::is_local_shim_loaded();
		}

		void apply_local_offline_mode_patches()
		{
			// Skip XLive-backed playlist/stat downloads. This lets local map bring-up
			// proceed without waiting on online storage checks.
			utils::hook::jump(game::game_offset(0x10240B30), ret_one);
			utils::hook::jump(game::game_offset(0x10240A30), ret_one);

			// Skip the XSessionCreate zero-session-id failure branch that raises
			// XBOXLIVE_NETCONNECTION during local session startup.
			utils::hook::nop(game::game_offset(0x102489A1), 5);

			console::info("[patches - offline] online storage/session checks bypassed\n");
		}

		using cl_parse_server_message_huffman_t = unsigned int(__cdecl*)(int, std::uint32_t*);
		utils::hook::detour cl_parse_server_message_huffman_hook;
		unsigned int __cdecl CL_ParseServerMessage_huffman_guard(int a1, std::uint32_t* a2)
		{
			const cl_parse_server_message_huffman_t original = reinterpret_cast<cl_parse_server_message_huffman_t>(cl_parse_server_message_huffman_hook.get_original());

			if (!a2)
			{
				return original(a1, a2);
			}

			if (a2[5] < a2[7])
			{
				game::Com_Error(
					(int)".\\cl_parse_mp.cpp",
					1243,
					1,
					(char*)"Huffman compressed msg cursor underflow detected\n");
				return 0;
			}

			const auto compressed_bytes = static_cast<std::size_t>(a2[5] - a2[7]);
			if (compressed_bytes > k_huffman_max_compressed_bytes)
			{
				game::Com_Error(
					(int)".\\cl_parse_mp.cpp",
					1243,
					1,
					(char*)"Huffman compressed msg exceeded safe decode limit (%u > %u)\n",
					static_cast<unsigned int>(compressed_bytes),
					static_cast<unsigned int>(k_huffman_max_compressed_bytes));
				return 0;
			}

			return original(a1, a2);
		}

		using ui_replace_directive_t = char*(__fastcall*)(int, char*, int, unsigned __int8);
		utils::hook::detour ui_replace_directive_hook;
		char* __fastcall UI_ReplaceDirective_guard(int ArgList, char* a2, int a3, unsigned __int8 a4)
		{
			const ui_replace_directive_t original = reinterpret_cast<ui_replace_directive_t>(ui_replace_directive_hook.get_original());
			const auto* const arg_list = reinterpret_cast<const char*>(ArgList);
			if (bounded_length(arg_list, k_ui_replace_directive_max_len + 1) > k_ui_replace_directive_max_len
				|| bounded_length(a2, k_ui_replace_directive_max_len + 1) > k_ui_replace_directive_max_len)
			{
				game::Com_Printf(0, "UI_ReplaceDirective: rejected oversized directive input\n");
				return a2;
			}

			return original(ArgList, a2, a3, a4);
		}

		using party_atomic_host_handle_member_join_t = int(__cdecl*)(char, std::uint32_t*, int, __int64, int, std::uint32_t*);
		utils::hook::detour party_atomic_host_handle_member_join_hook;
		int __cdecl PartyAtomicHost_HandleMemberJoin_guard(char a1, std::uint32_t* a2, int a3, __int64 a4, int a5, std::uint32_t* a6)
		{
			const party_atomic_host_handle_member_join_t original = reinterpret_cast<party_atomic_host_handle_member_join_t>(party_atomic_host_handle_member_join_hook.get_original());
			if (!a2 || !a6)
			{
				return original(a1, a2, a3, a4, a5, a6);
			}

			if (a6[5] < a6[7])
			{
				game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin: rejected malformed message cursor\n");
				return 0;
			}

			const auto unread_bytes = static_cast<std::size_t>(a6[5] - a6[7]);
			if (unread_bytes > k_party_member_join_max_message_bytes)
			{
				game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin: rejected oversized message (%zu bytes)\n", unread_bytes);
				return 0;
			}

			return original(a1, a2, a3, a4, a5, a6);
		}

		bool PartyAtomicHost_HandleMemberJoin_self_test()
		{
			std::uint32_t packet_cursor[8]{};
			std::uint32_t join_state[8]{};

			packet_cursor[5] = 0;
			packet_cursor[7] = 1;

			const auto result = PartyAtomicHost_HandleMemberJoin_guard(0, &join_state[0], 0, 0, 0, &packet_cursor[0]);
			if (result != 0)
			{
				game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin self-test failed\n");
				return false;
			}

			game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin self-test passed\n");
			return true;
		}

		bool UI_ReplaceDirective_self_test()
		{
			char oversized[0x110]{};
			std::memset(oversized, 'A', sizeof(oversized) - 1);

			volatile std::uint32_t canary_before = 0xDEADBEEF;
			char output[0x110]{};
			volatile std::uint32_t canary_after = 0xCAFEBABE;

			const auto result = UI_ReplaceDirective_guard(reinterpret_cast<std::uintptr_t>(oversized), output, 0, 0);
			if (result != output)
			{
				game::Com_Printf(0, "UI_ReplaceDirective self-test failed: Wrong return pointer\n");
				return false;
			}

			if (canary_before != 0xDEADBEEF || canary_after != 0xCAFEBABE)
			{
				game::Com_Printf(0, "UI_ReplaceDirective self-test failed: Stack overflow detected!\n");
				return false;
			}

			if (output[sizeof(output) - 1] != '\0')
			{
				game::Com_Printf(0, "UI_ReplaceDirective self-test failed: Output not null-terminated\n");
				return false;
			}

			game::Com_Printf(0, "UI_ReplaceDirective self-test passed\n");
			return true;
		}

		void register_security_guard_self_test()
		{
			command::add("securityGuardSelfTest", [](const command::params&)
			{
				const auto party_ok = PartyAtomicHost_HandleMemberJoin_self_test();
				const auto ui_ok = UI_ReplaceDirective_self_test();
				game::Com_Printf(0, "securityGuardSelfTest: party=%s ui=%s\n",
					party_ok ? "pass" : "fail",
					ui_ok ? "pass" : "fail");
			});
		}

		bool dvar_enabled(const char* name)
		{
			const auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return false;
			}

			switch (dvar->type)
			{
			case game::dvar_type::boolean:
				return dvar->current.enabled;
			case game::dvar_type::integer:
				return dvar->current.integer != 0;
			case game::dvar_type::value:
				return dvar->current.value != 0.0f;
			case game::dvar_type::string:
			case game::dvar_type::enumeration:
				if (!dvar->current.string)
				{
					return false;
				}

				return dvar->current.string[0] != '\0'
					&& strcmp(dvar->current.string, "0") != 0
					&& _stricmp(dvar->current.string, "false") != 0
					&& _stricmp(dvar->current.string, "off") != 0;
			default:
				return dvar->current.integer != 0;
			}
		}

		int dvar_int_value(const char* name, const int fallback = 0)
		{
			const auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return fallback;
			}

			switch (dvar->type)
			{
			case game::dvar_type::boolean:
				return dvar->current.enabled ? 1 : 0;
			case game::dvar_type::integer:
				return dvar->current.integer;
			case game::dvar_type::value:
				return static_cast<int>(dvar->current.value);
			case game::dvar_type::string:
			case game::dvar_type::enumeration:
				return dvar->current.string ? std::atoi(dvar->current.string) : fallback;
			default:
				return dvar->current.integer;
			}
		}

		void make_dvar_saved_and_writable(const char* name)
		{
			auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | static_cast<std::uint16_t>(game::dvar_flags::saved));
		}

		void make_dvar_debug_writable(const char* name)
		{
			auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only
					| game::dvar_flags::write_protected
					| game::dvar_flags::latched
					| game::dvar_flags::cheat_protected);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | static_cast<std::uint16_t>(game::dvar_flags::saved));
		}

		game::dvar_s* __cdecl register_fullscreen_for_window_parms(const char* name)
		{
			// QoS 1.1, 0x103BE15B: register before window parms are read, including
			// the first launch. Use native registration to convert a config-created
			// string and apply latched values without allocating a duplicate dvar.
			const auto target = game::game_offset(0x10278E60);
			const auto* description = "Display game full screen";
			const int flags = game::dvar_flags::saved | game::dvar_flags::latched;
			game::dvar_s* result;
			__asm
			{
				push flags
				push 1
				push name
				xor ecx, ecx
				mov edx, description
				call target
				add esp, 0Ch
				mov result, eax
			}
			*reinterpret_cast<game::dvar_s**>(game::game_offset(0x113EFA78)) = result;
			return result;
		}

		void apply_video_dvar_patches()
		{
			// Verified against QoS 1.1 and COD4 Mac Com_Frame_Try_Block_Function,
			// R_BeginRegistration and R_SetD3DPresentParameters (2026-09-10).
			// Keep native registration calls AND their EAX consumers intact.
			// In particular, WM_CREATE stores EAX into the fullscreen pointer.
			// Runtime regression: r_fullscreen 0/1 + vid_restart, repeated restarts,
			// relaunch with saved windowed config, and Alt-Tab/device recovery.
			// Check videoInfo after each; test com_maxfps 30/60/125/250/0 with
			// r_vsync 0 + vid_restart. These patches can go when these native
			// registration/window-parms routines are replaced in source.
			const auto check = [](const std::uintptr_t address, const char* bytes, const std::size_t size)
			{
				if (std::memcmp(reinterpret_cast<const void*>(game::game_offset(address)), bytes, size) != 0)
				{
					throw std::runtime_error("Unsupported engine instructions for video dvar patches");
				}
			};
			check(0x103BE15B, "\xE8\xE0\x7E\xEB\xFF", 5);
			check(0x103BE16D, "\xE8\x2E\x6A\xEB\xFF", 5);
			check(0x102C448F, "\x6A\x40", 2);
			check(0x102C44A8, "\xE8\xB3\x49\xFB\xFF", 5);
			check(0x103F6960, "\x6A\x40", 2);
			check(0x103F6969, "\x6A\x1E", 2);

			utils::hook::call(game::game_offset(0x103BE15B), register_fullscreen_for_window_parms);
			utils::hook::nop(game::game_offset(0x103BE16D), 5);
			utils::hook::set<std::uint8_t>(game::game_offset(0x102C4490),
				game::dvar_flags::saved | game::dvar_flags::latched);
			utils::hook::set<std::uint8_t>(game::game_offset(0x103F6961), game::dvar_flags::saved);
			utils::hook::set<std::uint8_t>(game::game_offset(0x103F696A), 85);
		}

		HWND __stdcall create_window_ex_stub(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
		{
			const auto named_class = class_name && !IS_INTRESOURCE(class_name);
			if (named_class && !strcmp(class_name, "007 Splash Screen"))
			{
				// Keep QoS' later GetWindowRect/SetWindowPos resize in physical pixels.
				// Otherwise a DPI-virtualized startup thread scales 768x480 twice.
				resources::prepare_splash_thread();
				if (resources::get_splash_dimensions(width, height))
				{
					x = (GetSystemMetrics(SM_CXFULLSCREEN) - width) / 2;
					y = (GetSystemMetrics(SM_CYFULLSCREEN) - height) / 2;
				}
			}
			else if (named_class && !strcmp(class_name, "Static") && parent)
			{
				char parent_class[64]{};
				if (GetClassNameA(parent, parent_class, static_cast<int>(sizeof(parent_class)))
					&& !strcmp(parent_class, "007 Splash Screen"))
				{
					resources::get_splash_dimensions(width, height);
					class_name = resources::splash_control_class();
					// The replacement child handles STM_SETIMAGE and paints the
					// complete bitmap itself. Remove Static-specific style bits.
					if (strcmp(class_name, "Static")) style &= ~0xFFFFu;
				}
			}
			else if (named_class && !strcmp(class_name, "JB_MP"))
			{
				window_name = "Project: Consolation - Multiplayer";

				const auto fullscreen = dvar_enabled("r_fullscreen");
				const bool borderless = dvar_enabled("r_borderless");

				if (!fullscreen)
				{
					x = dvar_int_value("vid_xpos", x);
					y = dvar_int_value("vid_ypos", y);
				}

				if (!fullscreen && borderless)
				{
					// The engine already enlarged width/height for the original frame.
					// Remove that padding before changing to a popup window.
					RECT frame{ 0, 0, 0, 0 };
					if (AdjustWindowRectEx(&frame, style, menu != nullptr, ex_style))
					{
						width -= frame.right - frame.left;
						height -= frame.bottom - frame.top;
					}
					style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
					style |= WS_POPUP;
					ex_style &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
					ex_style |= WS_EX_APPWINDOW;
				}
			}
			return CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, inst, param);
		}

		template <typename T>
		T* find_dvar(std::unordered_map<std::string, T>& map, const std::string& name)
		{
			auto i = map.find(name);
			if (i != map.end())
			{
				return &i->second;
			}

			return nullptr;
		}

		bool find_dvar(std::unordered_set<std::string>& set, const std::string& name)
		{
			return set.find(name) != set.end();
		}

		utils::hook::detour dvar_registernew_hook;
		game::dvar_s* Dvar_RegisterNew_Stub(const char* dvarName, game::DvarType type, unsigned short flags, char* desc, int unk, game::DvarValue value, game::DvarLimits domain)
		{
			if (type == game::DVAR_TYPE_FLOAT_2 && !_stricmp(dvarName, "cg_debugInfoCornerOffset"))
			{
				value.vector[0] = 0.0f;
				value.vector[1] = 0.0f;
			}

			if (type == game::DVAR_TYPE_BOOL)
			{
				auto* var = find_dvar(dvars::overrides::register_bool_overrides, dvarName);
				if (var)
				{

					value.enabled = var->value;
					flags = var->flags;
				}

			}

			if (type == game::DVAR_TYPE_INT)
			{
				auto* var = find_dvar(dvars::overrides::register_int_overrides, dvarName);
				if (var)
				{
					value.integer = var->value;
					domain.integer.max = var->max;
					domain.integer.min = var->min;
					flags = var->flags;
				}
			}

			if (type == game::DVAR_TYPE_ENUM)
			{
				if (!_stricmp(dvarName, "cg_drawFPS"))
				{
					const auto writable_flags = flags
						& ~static_cast<unsigned short>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);
					flags = static_cast<unsigned short>(writable_flags | static_cast<unsigned short>(game::dvar_flags::saved));
					domain.enumeration.stringCount = static_cast<int>(std::size(cg_draw_fps_modes));
					domain.enumeration.strings = cg_draw_fps_modes;
				}
			}

			if (type == game::DVAR_TYPE_FLOAT)
			{
				auto* var = find_dvar(dvars::overrides::register_float_overrides, dvarName);
				if (var)
				{
					value.value = var->value;
					domain.value.max = var->max;
					domain.value.min = var->min;
					flags = var->flags;
				}
			}

			if (type == game::DVAR_TYPE_STRING)
			{
				auto* var = find_dvar(dvars::overrides::register_string_overrides, dvarName);
				if (var)
				{
					value.string = var->value.c_str();
					flags = static_cast<unsigned short>(var->flags);
				}
			}

			return dvar_registernew_hook.invoke<game::dvar_s*>(dvarName, type, flags, desc, unk, value, domain);
		}

		utils::hook::detour BG_GetPlayerJumpHeight_hook;
		float BG_GetPlayerJumpHeight_stub(int a1)
		{
			auto jump_height = game::Dvar_FindVar("jump_height");

			if (!jump_height)
				return BG_GetPlayerJumpHeight_hook.invoke<float>(a1);

			return jump_height->current.value;
		}

		utils::hook::detour BG_GetPlayerSpeed_hook;
		int BG_GetPlayerSpeed_stub(int a1)
		{
			auto g_speed = game::Dvar_FindVar("g_speed");

			if (!g_speed)
				return BG_GetPlayerSpeed_hook.invoke<int>(a1);

			return g_speed->current.integer;
		}

		utils::hook::detour r_lodScale_hook;
		void r_lodScale_stub(const float value)
		{
			const auto* const dvar = game::Dvar_FindVar("r_lodScale");
			// The renderer re-applies this from r_lodScaleParam during init and
			// asset load. If the user has already modified the dvar, keep their
			// value instead of snapping back to the param-derived default.
			if (dvar && dvar->modified)
			{
				return;
			}

			r_lodScale_hook.invoke<void>(value);
		}


		float __cdecl Jump_GetLandFactor(DWORD* ps)
		{
			__int64 v1; // r10
			double v2; // fp1

			auto jump_slowdownEnable = game::Dvar_FindVar("jump_slowdownEnable");
			if (jump_slowdownEnable->current.enabled)
			{
				if (*(DWORD*)(ps + 24) < 1700)
				{
					v1 = *(DWORD*)(ps + 24);
					v2 = (float)((float)((float)v1 * (float)0.00088235294) + (float)1.0);
				}
				else
				{
					v2 = 2.5;
				}
			}
			else
			{
				v2 = 1.0;
			}
			return *((float*)&v2 + 1);
		}

		utils::hook::detour Jump_Start_hook;
		int Jump_Start_stub(int unused, int unused2, DWORD* pml_t)
		{
			DWORD* pmove_t{};
			float jump_height = game::Dvar_FindVar("jump_height")->current.value;

			_asm
			{
				mov  edi, DWORD PTR[edi]; edi = *edi
				mov  DWORD PTR[pmove_t], edi
			}

			auto v3 = *pmove_t;
			auto gravity = *(int*)(*pmove_t + 0x68);
			auto calculatedGravity = (double)gravity * (jump_height + jump_height);

			if ((*(DWORD*)(*pmove_t + 12) & 0x4000) != 0 && *(DWORD*)(v3 + 24) <= 1800)
			{
				auto landFactor = Jump_GetLandFactor(pmove_t);
				calculatedGravity = (float)((float)calculatedGravity / (float)landFactor);
			}

			pml_t[12] = 0;
			pml_t[13] = 0;
			pml_t[11] = 0;

			auto zOrigin = *(float*)(v3 + 40);
			*(DWORD*)(v3 + 128) = 1023; // groundEntityNum
			auto serverTime = pmove_t[1];
			*(float*)(v3 + 140) = zOrigin;

			*(DWORD*)(v3 + 136) = serverTime;
			auto v9 = sqrt(calculatedGravity);
			auto v11 = *(DWORD*)(v3 + 12) & 0xFFFFFE7F | 0x4000;
			*(float*)(v3 + 52) = v9;
			*(DWORD*)(v3 + 12) = v11;
			*(DWORD*)(v3 + 24) = 0;
			*(DWORD*)(v3 + 3900) = 0;

			auto v13 = game::Dvar_FindVar("jump_spreadAdd")->current.value;
			auto v14 = *(float*)(v3 + 4340) + v13;
			*(float*)(v3 + 4340) = v14;
			if (v14 > 255.0)
				*(DWORD*)(v3 + 4340) = 255.0;

			return v13;
		}
	}

	class component final : public component_interface
	{
		bool timer_period_active_ = false;

	public:
		void post_load() override
		{
			apply_input_and_overhead_patches();
			apply_video_dvar_patches();
			apply_cinematic_stats_guard();
			disable_startup_remote_screen_sync();
			apply_missing_voice_engine_guard();
			apply_private_match_unpause();
			apply_cg_draw_fps_modes();
			apply_noclip_vertical_input();
			// branding - intercept import for CreateWindowExA to change window title
			utils::hook::set(game::game_offset(0x1047627C), create_window_ex_stub);

			// nop call to Com_Printf for "SCALEFORM: %s" messages
			utils::hook::nop(game::game_offset(0x1000230F), 0x05); // TODO: Dvar toggle? Could be useful info
			utils::hook::nop(game::game_offset(0x102E1284), 0x05);
			// nop above call to Com_Printf for "unknown UI script %s in block:\n%s\n"

			// keep the registered version dvar value instead of letting stock init overwrite it
			utils::hook::nop(game::game_offset(0x103F9E53), 0x05);

			// stop an engine UI path from intentionally breaking into the debugger
			utils::hook::nop(game::game_offset(0x1027D3C4), 0x05);

			// various hooks to return dvar functionality, thanks to Liam
			BG_GetPlayerJumpHeight_hook.create(game::game_offset(0x101E6900), BG_GetPlayerJumpHeight_stub);
			BG_GetPlayerSpeed_hook.create(game::game_offset(0x101E6930), BG_GetPlayerSpeed_stub);
			r_lodScale_hook.create(game::game_offset(0x10279010), r_lodScale_stub);

			Jump_Start_hook.create(game::game_offset(0x101DB390), Jump_Start_stub);

			const auto offline_mode = local_offline_mode_requested();
			dvars::overrides::register_bool("cl_offlineMode", offline_mode, game::dvar_flags::read_only);

			// support local/xliveless map bring-up without online storage/session checks
			if (offline_mode)
			{
				apply_local_offline_mode_patches();
			}

#ifdef XLIVELESS
			if (!offline_mode)
			{
				apply_local_offline_mode_patches();
			}
#endif

			dvars::overrides::register_bool("sv_cheats", 1, game::dvar_flags::none);
			dvars::overrides::register_string("version", build_version_string(),
				static_cast<unsigned int>(game::dvar_flags::server_info | game::dvar_flags::read_only));
			dvars::overrides::register_string("shortversion", build_shortversion_string(),
				static_cast<unsigned int>(game::dvar_flags::server_info | game::dvar_flags::read_only));
			dvars::overrides::register_string("gamename", "Project: Consolation",
				static_cast<unsigned int>(game::dvar_flags::read_only));
			dvars::overrides::register_string("gamedate", build_game_date_string(),
				static_cast<unsigned int>(game::dvar_flags::read_only));
			//dvars::overrides::register_float("r_lodScale", 0, 0, 3, game::dvar_flags::saved); //doesn't save
			//dvars::overrides::register_float("jump_height", 39.0, 0, 1000, game::dvar_flags::saved); //adjusted to 39 to allow cod4-like jump onto ledges

			
			dvar_registernew_hook.create(game::Dvar_RegisterNew, Dvar_RegisterNew_Stub);

			scheduler::once([this]
			{
				pm_adsStopsSprint = dvars::Dvar_RegisterBool("pm_adsStopsSprint", 0,
					"ADS ends sprint in shared player movement (0 = stock QoS).", game::dvar_flags::replicated);
				pm_allowProne = dvars::Dvar_RegisterInt("pm_allowProne",
					"Prone permission: 0 disabled, 1 enabled with native clearance, 2 stock weapon rules.",
					2, 0, 2, game::dvar_flags::replicated);
				pm_adsExitOnDamage = dvars::Dvar_RegisterBool("pm_adsExitOnDamage", 1,
					"Allow stock scope cancellation on damage (0 keeps ADS; recoil is unchanged).", game::dvar_flags::replicated);
				pm_allowCover = dvars::Dvar_RegisterBool("pm_allowCover", 1,
					"Allow native cover entry (0 starts a native exit from active cover).", game::dvar_flags::replicated);
				pm_mantleFirstPerson = dvars::Dvar_RegisterBool("pm_mantleFirstPerson", 0,
					"Use first-person camera during native mantle traversals (0 = stock camera).", game::dvar_flags::replicated);
				pm_climbFirstPerson = dvars::Dvar_RegisterBool("pm_climbFirstPerson", 0,
					"Use first-person camera on native ladders, ledges and pipes (0 = stock camera).", game::dvar_flags::replicated);
				pm_airborneBobScale = dvars::Dvar_RegisterFloat("pm_airborneBobScale",
					"Airborne movement camera bob scale: 0 suppresses, 1 restores stock; grounded bob is unchanged.",
					1.0f, 0.0f, 1.0f, game::dvar_flags::replicated);
				game::DvarValue movement_default{};
				movement_default.integer = 0;
				game::DvarLimits movement_domain{};
				movement_domain.enumeration.stringCount = static_cast<int>(std::size(movement_mode_names));
				movement_domain.enumeration.strings = movement_mode_names;
				pm_movement_mode = game::Dvar_RegisterNew("pm_movement_mode", game::DVAR_TYPE_ENUM,
					game::dvar_flags::replicated, "Movement preset: stock QoS or experimental COD4-style iw3.",
					0, movement_default, movement_domain);
				apply_movement_preset();
				scheduler::loop(apply_movement_preset, scheduler::main);
				overhead_font_dvar = dvars::Dvar_RegisterInt("cg_overheadNamesFont",
					"Overhead font: 0 native, 1 normal, 2 big, 3 small, 4 bold (missing fonts use native).",
					0, 0, 4, game::dvar_flags::saved | game::dvar_flags::cheat_protected);
				command::add("overheadFontNext", [](const command::params&)
				{
					// Dvar_SetString uses the INTERNAL engine setter, so the command
					// needs its own gate as well as the dvar's native cheat flag.
					const auto* const cheats = game::Dvar_FindVar("sv_cheats");
					if (!cheats || !cheats->current.enabled)
					{
						console::info("overheadFontNext: cheats are not enabled\n");
						return;
					}
					const int next = overhead_font_dvar ? (overhead_font_dvar->current.integer + 1) % 5 : 0;
					game::Dvar_SetString("cg_overheadNamesFont", utils::string::va("%d", next));
					console::info("cg_overheadNamesFont = %d\n", next);
				});
				pm_noclipScale = dvars::Dvar_RegisterFloat("pm_noclipScale",
					"Noclip movement speed multiplier (1 = native speed).",
					1.0f, 0.0f, 20.0f, game::dvar_flags::saved);

				// Run outside DLL initialization. The native frame loop uses
				// timeGetTime and Sleep(1), but this image imports no timeBeginPeriod.
				// Request precision in this process to avoid coarse timer pacing.
				timer_period_active_ = timeBeginPeriod(1) == TIMERR_NOERROR;

				dvars::replace_dvar_at(game::game_offset(0x103AF41F), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054688)),
					dvars::make_float("r_lodScale", "Scale the level of detail distance (larger reduces detail)", 0.0f, 0.0f, 3.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x102BE942), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148BECC)),
					dvars::make_float("cg_fovScale", "Scale applied to the field of view", 1.0f, 0.0f, 2.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x102BE908), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148F6A4)),
					dvars::make_float("cg_fov", "The field of view angle in degrees", 65.0f, 0.0f, 160.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x10321250), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11260BD0)),
					dvars::make_float("input_viewSensitivity", "Mouse sensitivity", 1.0f, 0.01f, 30.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x101DB65A), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x118EE1C0)),
					dvars::make_float("jump_height", "The maximum height of a player's jump", 41.0f, 0.0f, 1000.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x103B2260), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054944)),
					dvars::make_int("developer", "Enable development environment", 0, 0, 2, game::dvar_flags::none));

				make_dvar_saved_and_writable("sv_cheats");
				make_dvar_saved_and_writable("vid_xpos");
				make_dvar_saved_and_writable("vid_ypos");

				//debug block sv_cheats
#ifdef DEBUG
				utils::hook::nop(game::game_offset(0x101AB211), 5);
				// The removed registration call normally leaves its dvar in EAX. If
				// the following store remains, map/server initialization writes the
				// unrelated stale EAX value over the native cheat-gate pointer.
				utils::hook::nop(game::game_offset(0x101AB22A), 5);
				utils::hook::nop(game::game_offset(0x10245A2A), 5);
				utils::hook::nop(game::game_offset(0x10245A2F), 5);

				auto* const sv_cheats = dvars::Dvar_RegisterBool("sv_cheats", 1, "Enable Cheats", game::dvar_flags::none);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x11A343C0)) = sv_cheats;
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x1149FCD8)) = sv_cheats;
				make_dvar_debug_writable("r_fullbright");
#endif
			}, scheduler::main);

			cl_parse_server_message_huffman_hook.create(game::game_offset(0x1030D960), CL_ParseServerMessage_huffman_guard);
			ui_replace_directive_hook.create(game::game_offset(0x102BB870), UI_ReplaceDirective_guard);
			party_atomic_host_handle_member_join_hook.create(game::game_offset(0x103087B0), PartyAtomicHost_HandleMemberJoin_guard);
			register_security_guard_self_test();
			command::add("videoInfo", [this](const command::params&)
			{
				// Compare requested/current dvars with the renderer's cached
				// presentation parameters, which are updated on creation AND reset.
				for (const auto* name : { "r_fullscreen", "r_vsync", "com_maxfps" })
				{
					const auto* dvar = game::Dvar_FindVar(name);
					if (dvar && (dvar->type == game::dvar_type::boolean || dvar->type == game::dvar_type::integer))
					{
						const bool boolean = dvar->type == game::dvar_type::boolean;
						game::Com_Printf(0, "%s: current=%i latched=%i flags=0x%04X\n", name,
							boolean ? dvar->current.enabled : dvar->current.integer,
							boolean ? dvar->latched.enabled : dvar->latched.integer,
							static_cast<unsigned int>(dvar->flags));
					}
				}
				const auto* present = reinterpret_cast<const D3DPRESENT_PARAMETERS*>(game::game_offset(0x10E271F4));
				game::Com_Printf(0, "Renderer cache: %ux%u windowed=%i refresh=%u interval=0x%08X timer1ms=%i\n",
					present->BackBufferWidth, present->BackBufferHeight, present->Windowed,
					present->FullScreen_RefreshRateInHz, present->PresentationInterval, timer_period_active_);
				if (present->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE)
				{
					game::Com_Printf(0, "VSync can limit FPS to the display refresh rate. Use r_vsync 0; vid_restart for higher FPS.\n");
				}
			});

			scheduler::loop([]
			{
				make_dvar_saved_and_writable("sv_cheats");
				make_dvar_saved_and_writable("vid_xpos");
				make_dvar_saved_and_writable("vid_ypos");
#ifdef DEBUG
				make_dvar_debug_writable("r_fullbright");
#endif
			}, scheduler::main, 250ms);
		}

		void pre_destroy() override
		{
			if (timer_period_active_)
			{
				timeEndPeriod(1);
				timer_period_active_ = false;
			}
		}
	};
}

REGISTER_COMPONENT(patches::component)
