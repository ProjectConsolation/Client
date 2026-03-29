#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>

// Uses game::entity_t, game::playerState_t, game::client_t, game::usercmd_t,
// game::clientState_t from structs.hpp (added via structs_addition.hpp).

namespace bots
{
	namespace
	{
		constexpr int   CLIENT_STRIDE = 688916;      // sizeof(client_t) effectively
		constexpr int   PS_ORIGIN_OFFSET = 0x20;        // playerState_t::origin[0]
		constexpr int   PS_MAXHEALTH_OFFSET = 0x32C4;      // playerState_t::maxHealth
		constexpr int   CLIENT_ENTITYPTR_OFF = 0x2128C;     // client_t: entity_t* for this slot
		constexpr int   SNAPINFO_STRIDE = 13504;
		constexpr int   SNAPINFO_DEAD_OFF = 0x3228;
		constexpr float IW_RAD_TO_ANGLE = 10430.378f;  // 32768 / PI
		constexpr int   MAX_CLIENTS = 18;

		// ── Dvars ─────────────────────────────────────────────────────────────
		game::dvar_s* bot_max_health = nullptr;

		// ── Runtime array bases ───────────────────────────────────────────────
		// g_entities is a static .data array; its IDA address IS the base.
		// g_clients holds a runtime pointer — read through it at call time.

		inline game::entity_t* g_entities()
		{
			return reinterpret_cast<game::entity_t*>(game::game_offset(0x11961F80));
		}
		inline std::uintptr_t g_clients()
		{
			return *reinterpret_cast<std::uintptr_t*>(game::game_offset(0x11CA5D8C));
		}
		inline std::uintptr_t g_snapshot_info()
		{
			return *reinterpret_cast<std::uintptr_t*>(game::game_offset(0x11A34458));
		}
		inline std::uint8_t* g_archived_snap_base()
		{
			return *reinterpret_cast<std::uint8_t**>(game::game_offset(0x112EBD18));
		}
		inline int g_archived_snap_stride()
		{
			return *reinterpret_cast<int*>(game::game_offset(0x112EBD1C));
		}
		inline game::dvar_s* sv_maxclients_dvar()
		{
			return *reinterpret_cast<game::dvar_s**>(game::game_offset(0x112EBEFC));
		}
		inline game::dvar_s* sv_bots_press_attack_dvar()
		{
			return *reinterpret_cast<game::dvar_s**>(game::game_offset(0x1130BF80));
		}

		// ── Typed struct accessors ────────────────────────────────────────────
		// entity_t stride = sizeof(entity_t) = 0x290, confirmed from IDB.

		inline game::entity_t* entity_at(int idx)
		{
			return g_entities() + idx;
		}
		inline game::playerState_t* entity_ps(game::entity_t* ent)
		{
			return ent->ps;
		}
		inline float ps_origin(const game::playerState_t* ps, int axis)
		{
			return *reinterpret_cast<const float*>(
				reinterpret_cast<const std::uint8_t*>(ps) + PS_ORIGIN_OFFSET + axis * 4
				);
		}
		inline std::uintptr_t client_at(int idx)
		{
			return g_clients() + static_cast<std::uintptr_t>(idx) * CLIENT_STRIDE;
		}
		inline game::clientState_t client_state(int idx)
		{
			return *reinterpret_cast<game::clientState_t*>(client_at(idx));
		}
		inline bool client_is_real_player(int idx)
		{
			// client_t::isRealPlayer at +0x20: 0 = bot, non-zero = real player
			return *reinterpret_cast<int*>(client_at(idx) + 0x20) != 0;
		}
		inline bool client_is_dead(int idx)
		{
			const auto snap = g_snapshot_info();
			if (!snap) return true;
			return *reinterpret_cast<int*>(
				snap + static_cast<std::uintptr_t>(idx) * SNAPINFO_STRIDE + SNAPINFO_DEAD_OFF
				) != 0;
		}

		// ── SV_ClientThinkReal ────────────────────────────────────────────────
		// __usercall: eax = client_t*, edi = usercmd_t*
		// Address resolved once in post_load() via game_offset and cached here.
		static void* s_client_think_real_addr = nullptr;

		void call_sv_client_think_real(std::uintptr_t client, game::usercmd_t* cmd)
		{
			__asm
			{
				push edi                    // save EDI explicitly before we touch it
				mov  eax, client
				mov  edi, cmd
				call s_client_think_real_addr
				pop  edi                    // restore EDI after the __usercall
			}
		}

		// ── Per-bot attack phase toggle ───────────────────────────────────────
		static bool s_attack_phase[MAX_CLIENTS] = {};

		// ── Patch 1: health init ──────────────────────────────────────────────
		// Hook site: CALL at 0x10414440, inside inlined tail of sub_102EF480.
		// ESI = clientIdx at that site (mov esi,eax @ 0x10414421).
		// SV_ClientEnterWorld (0x101AA270) preserves ESI.

		void fix_client_health_impl(int client_idx)
		{
			if (client_is_real_player(client_idx))
				return;

			const int maxhp = (bot_max_health && bot_max_health->current.integer > 0)
				? bot_max_health->current.integer
				: 100;

			game::entity_t* ent = entity_at(client_idx);
			if (!ent) return;

			ent->health = maxhp;

			game::playerState_t* ps = entity_ps(ent);
			if (!ps) return;

			// ps->health mirrors entity->health
			*reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(ps) + 0x1CC) = maxhp;
			// ps->maxHealth
			*reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(ps) + PS_MAXHEALTH_OFFSET) = maxhp;
		}

		void __cdecl client_enter_world_hook()
		{
			const int func_loc = game::game_offset(0x101AA270);

			__asm { call func_loc }

			int client_idx;
			__asm { mov client_idx, esi }

			fix_client_health_impl(client_idx);
		}

		// ── Patch 2: SV_BotThink replacement ─────────────────────────────────
		// ESI = client_t* on entry (__usercall from SV_BotFrame).

		void bot_think_impl(std::uintptr_t client);

		void __cdecl sv_bot_think_hook()
		{
			std::uintptr_t client;
			__asm { mov client, esi }

			// Guard: entity ptr at client+0x2128C must be non-null (bot has spawned).
			if (*reinterpret_cast<void**>(client + CLIENT_ENTITYPTR_OFF) == nullptr)
				return;

			bot_think_impl(client);
		}

		void bot_think_impl(std::uintptr_t client)
		{
			const auto clients_base = g_clients();
			if (!clients_base) return;

			const int bot_idx = static_cast<int>((client - clients_base) / CLIENT_STRIDE);

			// Build a zero usercmd and copy the bot's current weapon.
			game::usercmd_t cmd = {};

			{
				auto* arch = g_archived_snap_base();
				const int stride = g_archived_snap_stride();
				if (arch && stride > 0)
				{
					cmd.weapon = arch[static_cast<std::ptrdiff_t>(bot_idx) * stride + 0xF0];
				}
			}

			if (!client_is_dead(bot_idx))
			{
				const auto* maxcl_dvar = sv_maxclients_dvar();
				const int   maxcl = maxcl_dvar ? maxcl_dvar->current.integer : 12;

				game::entity_t* bot_ent = entity_at(bot_idx);
				game::playerState_t* bot_ps = bot_ent ? entity_ps(bot_ent) : nullptr;

				int   best_idx = -1;
				float best_dist2 = FLT_MAX;

				if (bot_ps)
				{
					const float bx = ps_origin(bot_ps, 0);
					const float by = ps_origin(bot_ps, 1);
					const float bz = ps_origin(bot_ps, 2);

					for (int i = 0; i < maxcl; ++i)
					{
						if (i == bot_idx)                                   continue;
						if (client_state(i) != game::CS_ACTIVE)             continue;
						if (!client_is_real_player(i))                      continue;

						game::entity_t* tgt_ent = entity_at(i);
						if (!tgt_ent || tgt_ent->health <= 0)               continue;

						game::playerState_t* tgt_ps = entity_ps(tgt_ent);
						if (!tgt_ps)                                         continue;

						const float dx = ps_origin(tgt_ps, 0) - bx;
						const float dy = ps_origin(tgt_ps, 1) - by;
						const float dz = ps_origin(tgt_ps, 2) - bz;
						const float d2 = dx * dx + dy * dy + dz * dz;

						if (d2 < best_dist2) { best_dist2 = d2; best_idx = i; }
					}
				}

				if (best_idx >= 0 && bot_ps)
				{
					game::playerState_t* tgt_ps = entity_ps(entity_at(best_idx));

					const float dx = ps_origin(tgt_ps, 0) - ps_origin(bot_ps, 0);
					const float dy = ps_origin(tgt_ps, 1) - ps_origin(bot_ps, 1);
					const float dz = ps_origin(tgt_ps, 2) - ps_origin(bot_ps, 2);
					const float hdist = std::sqrtf(dx * dx + dy * dy);

					cmd.angles[1] = static_cast<int>(std::atan2f(dy, dx) * IW_RAD_TO_ANGLE); // YAW
					cmd.angles[0] = static_cast<int>(std::atan2f(-dz, hdist) * IW_RAD_TO_ANGLE); // PITCH
					cmd.angles[2] = 0;

					cmd.forwardmove = 127;

					// Toggle BUTTON_ATTACK every frame for rising-edge fire detection.
					const auto* atk = sv_bots_press_attack_dvar();
					if (atk && atk->current.enabled)
					{
						const int idx = bot_idx < MAX_CLIENTS ? bot_idx : 0;
						s_attack_phase[idx] = !s_attack_phase[idx];
						if (s_attack_phase[idx])
							cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_ATTACK);
					}
				}
				else
				{
					const int idx = bot_idx < MAX_CLIENTS ? bot_idx : 0;
					s_attack_phase[idx] = false;
				}
			}

			// Mirror original SV_BotThink: client[2] = client[4] - 1.
			*reinterpret_cast<int*>(client + 0x08) =
				*reinterpret_cast<int*>(client + 0x10) - 1;

			call_sv_client_think_real(client, &cmd);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// Resolve SV_ClientThinkReal address once at load time.
			s_client_think_real_addr = reinterpret_cast<void*>(game::game_offset(0x102F0BD0));

			utils::hook::call(game::game_offset(0x10414440), client_enter_world_hook);
			utils::hook::jump(game::game_offset(0x102FA590), sv_bot_think_hook);

			scheduler::once([]
				{
					bot_max_health = dvars::Dvar_RegisterInt(
						"bot_maxHealth",
						"Maximum health for bots on spawn",
						100, 1, 1000,
						game::dvar_flags::saved
					);
				}, scheduler::main);

			scheduler::on_shutdown([]
				{
					std::memset(s_attack_phase, 0, sizeof(s_attack_phase));
					bot_max_health = nullptr;
				});
		}

		void pre_destroy() override
		{
			// Restore original bytes at both hook sites before DLL unloads,
			// preventing jumps into unmapped memory during CRT shutdown.
			// 0x10414440: E8 2B 5E D9 FF  (call SV_ClientEnterWorld / 0x101AA270)
			// 0x102FA590: 83 EC 30 83 BE  (sub esp,30h + start of cmp [esi+2128Ch],0)
			const auto health_site = game::game_offset(0x10414440);
			utils::hook::set<std::uint8_t>(health_site + 0, 0xE8);
			utils::hook::set<std::uint8_t>(health_site + 1, 0x2B);
			utils::hook::set<std::uint8_t>(health_site + 2, 0x5E);
			utils::hook::set<std::uint8_t>(health_site + 3, 0xD9);
			utils::hook::set<std::uint8_t>(health_site + 4, 0xFF);

			const auto botthink_site = game::game_offset(0x102FA590);
			utils::hook::set<std::uint8_t>(botthink_site + 0, 0x83);
			utils::hook::set<std::uint8_t>(botthink_site + 1, 0xEC);
			utils::hook::set<std::uint8_t>(botthink_site + 2, 0x30);
			utils::hook::set<std::uint8_t>(botthink_site + 3, 0x83);
			utils::hook::set<std::uint8_t>(botthink_site + 4, 0xBE);
		}
	};
}

REGISTER_COMPONENT(bots::component)