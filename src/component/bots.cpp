#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace bots
{
	namespace
	{
		constexpr int CLIENT_STRIDE = 688916;
		constexpr int PS_ORIGIN_OFFSET = 0x20;
		constexpr int PS_MAXHEALTH_OFFSET = 0x32C4;
		constexpr int CLIENT_ENTITYPTR_OFF = 0x2128C;
		constexpr int CLIENT_USERINFO_OFF = 1604;
		constexpr int SNAPINFO_STRIDE = 13504;
		constexpr int SNAPINFO_DEAD_OFF = 0x3228;
		constexpr float IW_RAD_TO_ANGLE = 10430.378f;
		constexpr int MAX_CLIENTS = 18;
		constexpr float MELEE_RANGE = 96.0f;
		constexpr float CROUCH_RANGE = 384.0f;
		constexpr float SPRINT_RANGE = 700.0f;
		constexpr float FIRE_RANGE = 1400.0f;

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
			return *reinterpret_cast<int*>(client_at(idx) + 0x20) != 0;
		}

		inline bool client_is_dead(int idx)
		{
			const auto snap = g_snapshot_info();
			if (!snap)
			{
				return true;
			}

			return *reinterpret_cast<int*>(
				snap + static_cast<std::uintptr_t>(idx) * SNAPINFO_STRIDE + SNAPINFO_DEAD_OFF
			) != 0;
		}

		inline const char* client_userinfo(int idx)
		{
			return reinterpret_cast<const char*>(client_at(idx) + CLIENT_USERINFO_OFF);
		}

		std::string get_info_value(const char* info, const char* key)
		{
			if (!info || !key)
			{
				return {};
			}

			const std::string text(info);
			size_t pos = 0;

			while (pos < text.size())
			{
				if (text[pos] == '\\')
				{
					++pos;
				}

				const auto key_end = text.find('\\', pos);
				if (key_end == std::string::npos)
				{
					break;
				}

				const auto value_end = text.find('\\', key_end + 1);
				const auto current_key = text.substr(pos, key_end - pos);
				const auto current_value = text.substr(
					key_end + 1,
					(value_end == std::string::npos ? text.size() : value_end) - (key_end + 1)
				);

				if (!_stricmp(current_key.c_str(), key))
				{
					return current_value;
				}

				if (value_end == std::string::npos)
				{
					break;
				}

				pos = value_end;
			}

			return {};
		}

		game::bot_team parse_team_value(const std::string& value)
		{
			if (value.empty())
			{
				return game::bot_team::unknown;
			}

			const auto lower = utils::string::to_lower(value);
			if (lower == "axis" || lower == "1")
			{
				return game::bot_team::axis;
			}

			if (lower == "allies" || lower == "2")
			{
				return game::bot_team::allies;
			}

			if (lower == "spectator" || lower == "3")
			{
				return game::bot_team::spectator;
			}

			if (lower == "free" || lower == "0" || lower == "none")
			{
				return game::bot_team::free;
			}

			return game::bot_team::unknown;
		}

		game::bot_team client_team(int idx)
		{
			const auto team_num = parse_team_value(get_info_value(client_userinfo(idx), "teamNum"));
			if (team_num != game::bot_team::unknown)
			{
				return team_num;
			}

			return parse_team_value(get_info_value(client_userinfo(idx), "team"));
		}

		bool should_target_client(int bot_idx, int target_idx)
		{
			if (target_idx == bot_idx)
			{
				return false;
			}

			if (client_state(target_idx) != game::CS_ACTIVE || client_is_dead(target_idx))
			{
				return false;
			}

			auto* const target_ent = entity_at(target_idx);
			if (!target_ent || target_ent->health <= 0)
			{
				return false;
			}

			const auto bot_team = client_team(bot_idx);
			const auto target_team = client_team(target_idx);

			if (bot_team == game::bot_team::unknown || bot_team == game::bot_team::free)
			{
				return target_team != game::bot_team::spectator;
			}

			if (target_team == game::bot_team::unknown || target_team == game::bot_team::free)
			{
				return true;
			}

			return target_team != game::bot_team::spectator && target_team != bot_team;
		}

		static void* s_client_think_real_addr = nullptr;
		static bool s_attack_phase[MAX_CLIENTS] = {};

		void call_sv_client_think_real(std::uintptr_t client, game::usercmd_t* cmd)
		{
			__asm
			{
				push edi
				mov eax, client
				mov edi, cmd
				call s_client_think_real_addr
				pop edi
			}
		}

		int get_bot_max_health()
		{
			return (dvars::bot_maxHealth && dvars::bot_maxHealth->current.integer > 0)
				? dvars::bot_maxHealth->current.integer
				: 100;
		}

		void sanitize_bot_health(int client_idx, bool force_full_health)
		{
			if (client_is_real_player(client_idx))
			{
				return;
			}

			const int maxhp = get_bot_max_health();
			auto* const ent = entity_at(client_idx);
			if (!ent)
			{
				return;
			}

			auto* const ps = entity_ps(ent);
			if (!ps)
			{
				return;
			}

			auto& ps_health = *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(ps) + 0x1CC);
			auto& ps_max_health = *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(ps) + PS_MAXHEALTH_OFFSET);
			ps_max_health = maxhp;

			if (force_full_health)
			{
				ent->health = maxhp;
				ps_health = maxhp;
				return;
			}

			if (!client_is_dead(client_idx))
			{
				if (ent->health <= 0 || ent->health > maxhp)
				{
					ent->health = maxhp;
				}

				if (ps_health <= 0 || ps_health > maxhp)
				{
					ps_health = ent->health;
				}
			}
			else
			{
				if (ent->health > maxhp)
				{
					ent->health = maxhp;
				}

				if (ps_health > maxhp)
				{
					ps_health = maxhp;
				}
			}
		}

		void __cdecl client_enter_world_hook()
		{
			const int func_loc = game::game_offset(0x101AA270);

			__asm { call func_loc }

			int client_idx = 0;
			__asm { mov client_idx, esi }

			sanitize_bot_health(client_idx, true);
		}

		void bot_think_impl(std::uintptr_t client);

		void __cdecl sv_bot_think_hook()
		{
			std::uintptr_t client = 0;
			__asm { mov client, esi }

			if (*reinterpret_cast<void**>(client + CLIENT_ENTITYPTR_OFF) == nullptr)
			{
				return;
			}

			bot_think_impl(client);
		}

		void bot_think_impl(std::uintptr_t client)
		{
			const auto clients_base = g_clients();
			if (!clients_base)
			{
				return;
			}

			const int bot_idx = static_cast<int>((client - clients_base) / CLIENT_STRIDE);
			sanitize_bot_health(bot_idx, false);

			game::usercmd_t cmd = {};

			{
				auto* const arch = g_archived_snap_base();
				const int stride = g_archived_snap_stride();
				if (arch && stride > 0)
				{
					cmd.weapon = arch[static_cast<std::ptrdiff_t>(bot_idx) * stride + 0xF0];
				}
			}

			if (!client_is_dead(bot_idx))
			{
				const auto* const maxcl_dvar = sv_maxclients_dvar();
				const int maxcl = maxcl_dvar ? maxcl_dvar->current.integer : 12;

				auto* const bot_ent = entity_at(bot_idx);
				auto* const bot_ps = bot_ent ? entity_ps(bot_ent) : nullptr;

				int best_idx = -1;
				float best_dist2 = FLT_MAX;

				if (bot_ps)
				{
					const float bx = ps_origin(bot_ps, 0);
					const float by = ps_origin(bot_ps, 1);
					const float bz = ps_origin(bot_ps, 2);

					for (int i = 0; i < maxcl; ++i)
					{
						if (!should_target_client(bot_idx, i))
						{
							continue;
						}

						auto* const tgt_ent = entity_at(i);
						auto* const tgt_ps = entity_ps(tgt_ent);
						if (!tgt_ps)
						{
							continue;
						}

						const float dx = ps_origin(tgt_ps, 0) - bx;
						const float dy = ps_origin(tgt_ps, 1) - by;
						const float dz = ps_origin(tgt_ps, 2) - bz;
						const float d2 = dx * dx + dy * dy + dz * dz;

						if (d2 < best_dist2)
						{
							best_dist2 = d2;
							best_idx = i;
						}
					}
				}

				if (best_idx >= 0 && bot_ps)
				{
					auto* const tgt_ps = entity_ps(entity_at(best_idx));
					const float dx = ps_origin(tgt_ps, 0) - ps_origin(bot_ps, 0);
					const float dy = ps_origin(tgt_ps, 1) - ps_origin(bot_ps, 1);
					const float dz = ps_origin(tgt_ps, 2) - ps_origin(bot_ps, 2);
					const float hdist = std::sqrtf(dx * dx + dy * dy);
					const float dist = std::sqrtf(dx * dx + dy * dy + dz * dz);
					const int behavior_tick = bot_ps->commandTime / 250;

					cmd.angles[1] = static_cast<int>(std::atan2f(dy, dx) * IW_RAD_TO_ANGLE);
					cmd.angles[0] = static_cast<int>(std::atan2f(-dz, hdist) * IW_RAD_TO_ANGLE);
					cmd.angles[2] = 0;

					cmd.forwardmove = 127;
					cmd.rightmove = (behavior_tick + bot_idx) & 1 ? 48 : -48;

					if (dist > SPRINT_RANGE)
					{
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_SPRINT);
					}

					if (dist < CROUCH_RANGE && ((behavior_tick + bot_idx) % 3 == 0))
					{
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_CROUCH);
					}

					if (dist <= MELEE_RANGE)
					{
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_MELEE_BREATH);
						cmd.forwardmove = 127;
					}
					else
					{
						if (dist > 256.0f)
						{
							cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_ADS);
						}

						const auto* const atk = sv_bots_press_attack_dvar();
						if (atk && atk->current.enabled && dist <= FIRE_RANGE)
						{
							const int idx = bot_idx < MAX_CLIENTS ? bot_idx : 0;
							s_attack_phase[idx] = !s_attack_phase[idx];
							if (s_attack_phase[idx])
							{
								cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_ATTACK);
							}
						}
					}
				}
				else
				{
					const int idx = bot_idx < MAX_CLIENTS ? bot_idx : 0;
					s_attack_phase[idx] = false;
				}
			}

			*reinterpret_cast<int*>(client + 0x08) = *reinterpret_cast<int*>(client + 0x10) - 1;
			call_sv_client_think_real(client, &cmd);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			s_client_think_real_addr = reinterpret_cast<void*>(game::game_offset(0x102F0BD0));

			utils::hook::call(game::game_offset(0x10414440), client_enter_world_hook);
			utils::hook::jump(game::game_offset(0x102FA590), sv_bot_think_hook);

			scheduler::on_shutdown([]
				{
					std::memset(s_attack_phase, 0, sizeof(s_attack_phase));
				});
		}

		void pre_destroy() override
		{
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
