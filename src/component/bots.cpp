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
		constexpr int CLIENT_LAST_USERCMD_OFF = 0x20E9C;
		constexpr int CLIENT_USERINFO_OFF = 1604;
		constexpr int SNAPINFO_STRIDE = 13504;
		constexpr int SNAPINFO_DEAD_OFF = 0x3228;
		constexpr int CONTENTS_SOLID_SHOT = 41969713;
		constexpr int ENTITYNUM_NONE = 1023;
		constexpr float IW_RAD_TO_ANGLE = 10430.378f;
		constexpr int MAX_CLIENTS = 18;
		constexpr int TARGET_MEMORY_MS = 5000;
		constexpr int SEARCH_UPDATE_MS = 900;
		constexpr int STUCK_REPATH_MS = 1400;
		constexpr int AIM_SETTLE_THRESHOLD = 1400;
		constexpr int FIRE_VISIBILITY_THRESHOLD = 3;
		constexpr int FIRE_ALIGNMENT_THRESHOLD = 1200;
		constexpr float BOT_EYE_HEIGHT = 56.0f;
		constexpr float TARGET_EYE_HEIGHT = 48.0f;
		constexpr float TARGET_TORSO_HEIGHT = 26.0f;
		constexpr float TARGET_SAMPLE_WIDTH = 10.0f;
		constexpr float COVER_OFFSET_SIDE = 96.0f;
		constexpr float COVER_OFFSET_BACK = 128.0f;
		constexpr float COVER_OFFSET_DIAGONAL = 72.0f;
		constexpr float COVER_COMMIT_RANGE = 80.0f;
		constexpr float MELEE_RANGE = 96.0f;
		constexpr float CROUCH_RANGE = 384.0f;
		constexpr float SPRINT_RANGE = 700.0f;
		constexpr float FIRE_RANGE = 1400.0f;
		constexpr float STUCK_DIST_EPSILON = 24.0f;

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

		inline game::usercmd_t* client_last_usercmd(int idx)
		{
			return reinterpret_cast<game::usercmd_t*>(client_at(idx) + CLIENT_LAST_USERCMD_OFF);
		}

		bool client_recently_fired(int idx, int command_time)
		{
			__try
			{
				auto* const cmd = client_last_usercmd(idx);
				if (!cmd)
				{
					return false;
				}

				if ((cmd->buttons & game::BUTTON_ATTACK) == 0)
				{
					return false;
				}

				const int age = command_time - cmd->serverTime;
				return age >= 0 && age <= 1200;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
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
			if (lower == "axis" || lower == "1"
				|| lower.find("axis") != std::string::npos
				|| lower.find("cell") != std::string::npos
				|| lower.find("enemy") != std::string::npos)
			{
				return game::bot_team::axis;
			}

			if (lower == "allies" || lower == "2"
				|| lower.find("allies") != std::string::npos
				|| lower.find("mi6") != std::string::npos
				|| lower.find("friendly") != std::string::npos)
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

			if (bot_team == game::bot_team::unknown && target_team == game::bot_team::unknown)
			{
				return false;
			}

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

		static std::uintptr_t s_client_think_real_addr = 0;
		static bool s_attack_phase[MAX_CLIENTS] = {};
		static int s_tracked_target[MAX_CLIENTS] = {};
		static int s_last_visible_time[MAX_CLIENTS] = {};
		static float s_last_seen_origin[MAX_CLIENTS][3] = {};
		static int s_last_cmd_yaw[MAX_CLIENTS] = {};
		static int s_last_cmd_pitch[MAX_CLIENTS] = {};
		static int s_search_yaw[MAX_CLIENTS] = {};
		static int s_next_search_time[MAX_CLIENTS] = {};
		static int s_last_progress_time[MAX_CLIENTS] = {};
		static float s_last_progress_origin[MAX_CLIENTS][3] = {};
		static int s_last_applied_bot_max_health = 0;

		int bot_state_index(int client_idx)
		{
			return (client_idx >= 0 && client_idx < MAX_CLIENTS) ? client_idx : -1;
		}

		int normalize_angle_units(int angle)
		{
			while (angle > 32768)
			{
				angle -= 65536;
			}

			while (angle < -32768)
			{
				angle += 65536;
			}

			return angle;
		}

		void clear_bot_target_state(int client_idx)
		{
			const int idx = bot_state_index(client_idx);
			if (idx < 0)
			{
				return;
			}

			s_attack_phase[idx] = false;
			s_tracked_target[idx] = -1;
			s_last_visible_time[idx] = 0;
			s_last_seen_origin[idx][0] = 0.0f;
			s_last_seen_origin[idx][1] = 0.0f;
			s_last_seen_origin[idx][2] = 0.0f;
			s_last_cmd_yaw[idx] = 0;
			s_last_cmd_pitch[idx] = 0;
			s_last_progress_time[idx] = 0;
			s_last_progress_origin[idx][0] = 0.0f;
			s_last_progress_origin[idx][1] = 0.0f;
			s_last_progress_origin[idx][2] = 0.0f;
		}

		void get_eye_position(const game::playerState_t* ps, float eye_height, float* out)
		{
			out[0] = ps_origin(ps, 0);
			out[1] = ps_origin(ps, 1);
			out[2] = ps_origin(ps, 2) + eye_height;
		}

		bool trace_passed(const float* start, const float* end, int pass_entity_num)
		{
			const int func_loc = game::game_offset(0x101AA870);
			const int pass_entity_num_2 = ENTITYNUM_NONE;
			const int content_mask = CONTENTS_SOLID_SHOT;
			int result = 0;

			__asm
			{
				push content_mask
				push pass_entity_num_2
				push pass_entity_num
				mov esi, end
				mov edi, start
				call func_loc
				add esp, 0xC
				mov result, eax
			}

			return result != 0;
		}

		float distance_squared_3d(const float* a, const float* b)
		{
			const float dx = b[0] - a[0];
			const float dy = b[1] - a[1];
			const float dz = b[2] - a[2];
			return dx * dx + dy * dy + dz * dz;
		}

		void copy_vec3(float* out, const float* in)
		{
			out[0] = in[0];
			out[1] = in[1];
			out[2] = in[2];
		}

		void set_vec3(float* out, float x, float y, float z)
		{
			out[0] = x;
			out[1] = y;
			out[2] = z;
		}

		float vector_length_2d(float x, float y)
		{
			return std::sqrtf(x * x + y * y);
		}

		float vector_length_3d(float x, float y, float z)
		{
			return std::sqrtf(x * x + y * y + z * z);
		}

		int vector_to_yaw_units(float x, float y)
		{
			return static_cast<int>(std::atan2f(y, x) * IW_RAD_TO_ANGLE);
		}

		void yaw_units_to_forward_right(int yaw_units, float* forward, float* right)
		{
			const float radians = static_cast<float>(yaw_units) / IW_RAD_TO_ANGLE;
			const float sin_yaw = std::sinf(radians);
			const float cos_yaw = std::cosf(radians);

			forward[0] = cos_yaw;
			forward[1] = sin_yaw;
			forward[2] = 0.0f;

			right[0] = -sin_yaw;
			right[1] = cos_yaw;
			right[2] = 0.0f;
		}

		void build_target_sample_points(const float* bot_eye, const game::playerState_t* target_ps, float samples[5][3])
		{
			float target_eye[3] = {};
			get_eye_position(target_ps, TARGET_EYE_HEIGHT, target_eye);

			const float dx = target_eye[0] - bot_eye[0];
			const float dy = target_eye[1] - bot_eye[1];
			const float horizontal = vector_length_2d(dx, dy);
			float right[3] = { 0.0f, 1.0f, 0.0f };

			if (horizontal > 0.001f)
			{
				right[0] = -dy / horizontal;
				right[1] = dx / horizontal;
			}

			copy_vec3(samples[0], target_eye);
			set_vec3(samples[1], target_eye[0] + right[0] * TARGET_SAMPLE_WIDTH, target_eye[1] + right[1] * TARGET_SAMPLE_WIDTH, target_eye[2]);
			set_vec3(samples[2], target_eye[0] - right[0] * TARGET_SAMPLE_WIDTH, target_eye[1] - right[1] * TARGET_SAMPLE_WIDTH, target_eye[2]);
			set_vec3(samples[3], ps_origin(target_ps, 0), ps_origin(target_ps, 1), ps_origin(target_ps, 2) + TARGET_TORSO_HEIGHT);
			set_vec3(samples[4], ps_origin(target_ps, 0), ps_origin(target_ps, 1), ps_origin(target_ps, 2) + 8.0f);
		}

		float sample_target_visibility(const float* bot_eye, int bot_idx, const game::playerState_t* target_ps)
		{
			float samples[5][3] = {};
			build_target_sample_points(bot_eye, target_ps, samples);

			int visible_samples = 0;
			for (const auto& sample : samples)
			{
				if (trace_passed(bot_eye, sample, bot_idx))
				{
					++visible_samples;
				}
			}

			return static_cast<float>(visible_samples) / 5.0f;
		}

		bool find_cover_goal(const float* bot_eye, int bot_idx, const float* enemy_eye, int enemy_idx, float* out_goal)
		{
			const float enemy_dir_x = enemy_eye[0] - bot_eye[0];
			const float enemy_dir_y = enemy_eye[1] - bot_eye[1];
			const float enemy_yaw = vector_to_yaw_units(enemy_dir_x, enemy_dir_y);
			float forward[3] = {};
			float right[3] = {};
			yaw_units_to_forward_right(enemy_yaw, forward, right);

			const float candidates[5][3] =
			{
				{ bot_eye[0] - right[0] * COVER_OFFSET_SIDE, bot_eye[1] - right[1] * COVER_OFFSET_SIDE, bot_eye[2] },
				{ bot_eye[0] + right[0] * COVER_OFFSET_SIDE, bot_eye[1] + right[1] * COVER_OFFSET_SIDE, bot_eye[2] },
				{ bot_eye[0] - forward[0] * COVER_OFFSET_BACK, bot_eye[1] - forward[1] * COVER_OFFSET_BACK, bot_eye[2] },
				{ bot_eye[0] - forward[0] * COVER_OFFSET_BACK - right[0] * COVER_OFFSET_DIAGONAL, bot_eye[1] - forward[1] * COVER_OFFSET_BACK - right[1] * COVER_OFFSET_DIAGONAL, bot_eye[2] },
				{ bot_eye[0] - forward[0] * COVER_OFFSET_BACK + right[0] * COVER_OFFSET_DIAGONAL, bot_eye[1] - forward[1] * COVER_OFFSET_BACK + right[1] * COVER_OFFSET_DIAGONAL, bot_eye[2] }
			};

			float best_dist2 = FLT_MAX;
			bool found_cover = false;

			for (const auto& candidate : candidates)
			{
				if (!trace_passed(bot_eye, candidate, bot_idx))
				{
					continue;
				}

				if (trace_passed(enemy_eye, candidate, enemy_idx))
				{
					continue;
				}

				const float dist2 = distance_squared_3d(bot_eye, candidate);
				if (dist2 < best_dist2)
				{
					best_dist2 = dist2;
					copy_vec3(out_goal, candidate);
					found_cover = true;
				}
			}

			return found_cover;
		}

		void set_move_toward(game::usercmd_t& cmd, int view_yaw, int move_yaw, std::int8_t forward_speed, std::int8_t side_speed)
		{
			const int delta = normalize_angle_units(move_yaw - view_yaw);
			const int abs_delta = std::abs(delta);

			if (abs_delta <= 4096)
			{
				cmd.forwardmove = forward_speed;
				cmd.rightmove = 0;
				return;
			}

			if (abs_delta >= 24576)
			{
				cmd.forwardmove = static_cast<std::int8_t>(-forward_speed / 2);
				cmd.rightmove = 0;
				return;
			}

			cmd.forwardmove = static_cast<std::int8_t>(forward_speed / 2);
			cmd.rightmove = delta > 0 ? side_speed : static_cast<std::int8_t>(-side_speed);
		}

		void apply_search_movement(game::usercmd_t& cmd, int bot_idx, int command_time)
		{
			const int state_idx = bot_state_index(bot_idx);
			if (state_idx < 0)
			{
				return;
			}

			if (command_time >= s_next_search_time[state_idx])
			{
				const int phase = command_time / SEARCH_UPDATE_MS;
				const int degrees = ((phase * 45) + bot_idx * 35) % 360;
				s_search_yaw[state_idx] = static_cast<int>((degrees * (32768.0f / 180.0f)) - 32768.0f);
				s_next_search_time[state_idx] = command_time + SEARCH_UPDATE_MS;
			}

			cmd.angles[1] = s_search_yaw[state_idx];
			cmd.angles[0] = 0;
			cmd.angles[2] = 0;
			cmd.forwardmove = 127;
			cmd.rightmove = 0;
			cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_SPRINT);
		}

		bool bot_is_stuck(int bot_idx, const game::playerState_t* bot_ps)
		{
			const int state_idx = bot_state_index(bot_idx);
			if (state_idx < 0 || !bot_ps)
			{
				return false;
			}

			float current_origin[3] =
			{
				ps_origin(bot_ps, 0),
				ps_origin(bot_ps, 1),
				ps_origin(bot_ps, 2)
			};

			if (s_last_progress_time[state_idx] == 0)
			{
				copy_vec3(s_last_progress_origin[state_idx], current_origin);
				s_last_progress_time[state_idx] = bot_ps->commandTime;
				return false;
			}

			if (distance_squared_3d(s_last_progress_origin[state_idx], current_origin) > (STUCK_DIST_EPSILON * STUCK_DIST_EPSILON))
			{
				copy_vec3(s_last_progress_origin[state_idx], current_origin);
				s_last_progress_time[state_idx] = bot_ps->commandTime;
				return false;
			}

			return (bot_ps->commandTime - s_last_progress_time[state_idx]) >= STUCK_REPATH_MS;
		}

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

		void sanitize_bot_health(int client_idx, bool force_full_health, bool assume_bot = false)
		{
			if (!assume_bot && client_is_real_player(client_idx))
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

		void sync_bot_health_from_dvar()
		{
			const int maxhp = get_bot_max_health();
			if (maxhp == s_last_applied_bot_max_health)
			{
				return;
			}

			const auto* const maxcl_dvar = sv_maxclients_dvar();
			const int maxcl = maxcl_dvar ? maxcl_dvar->current.integer : MAX_CLIENTS;

			for (int i = 0; i < maxcl; ++i)
			{
				if (client_state(i) < game::CS_CONNECTED || client_is_real_player(i))
				{
					continue;
				}

				sanitize_bot_health(i, true);
			}

			s_last_applied_bot_max_health = maxhp;
		}

		void __cdecl client_enter_world_hook()
		{
			const int func_loc = game::game_offset(0x101AA270);

			__asm { call func_loc }

			int client_idx = 0;
			__asm { mov client_idx, esi }

			s_last_applied_bot_max_health = get_bot_max_health();
			sanitize_bot_health(client_idx, true);
		}

		void bot_user_move_impl(std::uintptr_t client);

		void __cdecl sv_bot_user_move_hook()
		{
			std::uintptr_t client = 0;
			__asm { mov client, esi }

			if (*reinterpret_cast<void**>(client + CLIENT_ENTITYPTR_OFF) == nullptr)
			{
				return;
			}

			bot_user_move_impl(client);
		}

		void bot_user_move_impl(std::uintptr_t client)
		{
			const auto clients_base = g_clients();
			if (!clients_base)
			{
				return;
			}

			sync_bot_health_from_dvar();

			const int bot_idx = static_cast<int>((client - clients_base) / CLIENT_STRIDE);
			sanitize_bot_health(bot_idx, false, true);

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
				const int state_idx = bot_state_index(bot_idx);

				auto* const bot_ent = entity_at(bot_idx);
				auto* const bot_ps = bot_ent ? entity_ps(bot_ent) : nullptr;
				float bot_eye[3] = {};

				int best_visible_idx = -1;
				float best_visible_score = -FLT_MAX;
				int best_hidden_idx = -1;
				float best_hidden_score = -FLT_MAX;
				float best_visibility = 0.0f;

				if (bot_ps)
				{
					cmd.serverTime = bot_ps->commandTime;
					get_eye_position(bot_ps, BOT_EYE_HEIGHT, bot_eye);

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

						float target_eye[3] = {};
						get_eye_position(tgt_ps, TARGET_EYE_HEIGHT, target_eye);

						const float d2 = distance_squared_3d(bot_eye, target_eye);
						const float visibility = sample_target_visibility(bot_eye, bot_idx, tgt_ps);

						if (visibility > 0.0f)
						{
							float score = (visibility * 600000.0f) - d2;
							if (state_idx >= 0 && s_tracked_target[state_idx] == i)
							{
								score += 60000.0f;
							}

							if (score > best_visible_score)
							{
								best_visible_score = score;
								best_visible_idx = i;
								best_visibility = visibility;
							}
						}
						else
						{
							float score = -d2;
							if (state_idx >= 0 && s_tracked_target[state_idx] == i)
							{
								score += 50000.0f;
							}

							if (client_recently_fired(i, bot_ps->commandTime))
							{
								score += 85000.0f;
							}

							if (score > best_hidden_score)
							{
								best_hidden_score = score;
								best_hidden_idx = i;
							}
						}
					}
				}

				int target_idx = best_visible_idx;
				bool target_visible = best_visible_idx >= 0;
				float goal[3] = {};
				float enemy_eye[3] = {};

				if (bot_ps && state_idx >= 0)
				{
					if (target_visible)
					{
						s_tracked_target[state_idx] = target_idx;
						s_last_visible_time[state_idx] = bot_ps->commandTime;

						auto* const visible_ps = entity_ps(entity_at(target_idx));
						get_eye_position(visible_ps, TARGET_EYE_HEIGHT, s_last_seen_origin[state_idx]);
					}
					else if (s_tracked_target[state_idx] >= 0
						&& should_target_client(bot_idx, s_tracked_target[state_idx])
						&& (bot_ps->commandTime - s_last_visible_time[state_idx]) <= TARGET_MEMORY_MS)
					{
						target_idx = s_tracked_target[state_idx];
					}
					else if (best_hidden_idx >= 0)
					{
						target_idx = best_hidden_idx;
						s_tracked_target[state_idx] = target_idx;

						auto* const hidden_ps = entity_ps(entity_at(target_idx));
						if (hidden_ps)
						{
							get_eye_position(hidden_ps, TARGET_EYE_HEIGHT, s_last_seen_origin[state_idx]);
							s_last_visible_time[state_idx] = bot_ps->commandTime;
						}
					}
					else
					{
						clear_bot_target_state(bot_idx);
					}
				}

				if (target_idx >= 0 && bot_ps)
				{
					auto* const tgt_ps = entity_ps(entity_at(target_idx));
					if (!tgt_ps)
					{
						clear_bot_target_state(bot_idx);
						*reinterpret_cast<int*>(client + 0x08) = *reinterpret_cast<int*>(client + 0x10) - 1;
						call_sv_client_think_real(client, &cmd);
						return;
					}

					if (target_visible)
					{
						get_eye_position(tgt_ps, TARGET_EYE_HEIGHT, goal);
						copy_vec3(enemy_eye, goal);
					}
					else if (state_idx >= 0 && s_last_visible_time[state_idx] > 0
						&& (bot_ps->commandTime - s_last_visible_time[state_idx]) <= TARGET_MEMORY_MS)
					{
						goal[0] = s_last_seen_origin[state_idx][0];
						goal[1] = s_last_seen_origin[state_idx][1];
						goal[2] = s_last_seen_origin[state_idx][2];
						copy_vec3(enemy_eye, goal);
					}
					else
					{
						get_eye_position(tgt_ps, TARGET_EYE_HEIGHT, goal);
						copy_vec3(enemy_eye, goal);
					}

					float move_goal[3] = {};
					copy_vec3(move_goal, goal);

					bool moving_to_cover = false;
					const int bot_health = bot_ent ? bot_ent->health : get_bot_max_health();

					const float dx = goal[0] - bot_eye[0];
					const float dy = goal[1] - bot_eye[1];
					const float aim_z = target_visible
						? (ps_origin(tgt_ps, 2) + TARGET_TORSO_HEIGHT)
						: goal[2];
					const float dz = aim_z - bot_eye[2];
					const float hdist = vector_length_2d(dx, dy);
					const float dist = vector_length_3d(dx, dy, dz);
					const float move_dx = move_goal[0] - bot_eye[0];
					const float move_dy = move_goal[1] - bot_eye[1];
					const bool should_seek_cover = target_visible
						&& best_visibility >= 0.4f
						&& dist <= FIRE_RANGE
						&& bot_health <= std::max(35, get_bot_max_health() / 2);
					const int behavior_tick = bot_ps->commandTime / 250;
					const bool stuck = bot_is_stuck(bot_idx, bot_ps);
					const int desired_yaw = vector_to_yaw_units(dx, dy);
					const int desired_pitch = std::clamp(static_cast<int>(std::atan2f(-dz, hdist) * IW_RAD_TO_ANGLE), -1400, 1400);
					int move_yaw = vector_to_yaw_units(move_dx, move_dy);
					const int yaw_delta = state_idx >= 0 ? std::abs(normalize_angle_units(desired_yaw - s_last_cmd_yaw[state_idx])) : 0;
					const int pitch_delta = state_idx >= 0 ? std::abs(normalize_angle_units(desired_pitch - s_last_cmd_pitch[state_idx])) : 0;
					const bool aim_settled = yaw_delta <= AIM_SETTLE_THRESHOLD && pitch_delta <= AIM_SETTLE_THRESHOLD;

					if (stuck)
					{
						move_yaw = normalize_angle_units(move_yaw + ((behavior_tick + bot_idx) & 1 ? 8192 : -8192));
						moving_to_cover = false;
					}

					if (should_seek_cover)
					{
						float cover_goal[3] = {};
						if (find_cover_goal(bot_eye, bot_idx, enemy_eye, target_idx, cover_goal))
						{
							copy_vec3(move_goal, cover_goal);
							moving_to_cover = distance_squared_3d(bot_eye, cover_goal) > (COVER_COMMIT_RANGE * COVER_COMMIT_RANGE);
						}
					}

					cmd.angles[1] = desired_yaw;
					cmd.angles[0] = desired_pitch;
					cmd.angles[2] = 0;

					if (state_idx >= 0)
					{
						s_last_cmd_yaw[state_idx] = desired_yaw;
						s_last_cmd_pitch[state_idx] = desired_pitch;
					}

					set_move_toward(
						cmd,
						desired_yaw,
						move_yaw,
						target_visible ? static_cast<std::int8_t>(96) : static_cast<std::int8_t>(127),
						target_visible ? static_cast<std::int8_t>(48) : static_cast<std::int8_t>(32)
					);

					if (stuck)
					{
						cmd.forwardmove = ((behavior_tick + bot_idx) & 1) ? 127 : static_cast<std::int8_t>(-96);
						cmd.rightmove = ((behavior_tick + bot_idx) & 2) ? 96 : -96;
						if (bot_ps->groundEntityNum != ENTITYNUM_NONE)
						{
							cmd.upmove = 127;
						}
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_SPRINT);
					}

					if (target_visible && !moving_to_cover && dist < FIRE_RANGE && dist > 192.0f)
					{
						const int strafe_phase = (behavior_tick + bot_idx) % 4;
						if (strafe_phase == 0)
						{
							cmd.rightmove = 24;
						}
						else if (strafe_phase == 2)
						{
							cmd.rightmove = -24;
						}
						else
						{
							cmd.rightmove = 0;
						}
					}

					if (!target_visible || dist > SPRINT_RANGE)
					{
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_SPRINT);
					}

					if (!stuck && target_visible && dist < CROUCH_RANGE && dist > 160.0f && ((behavior_tick + bot_idx) % 5 == 0))
					{
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_CROUCH);
					}

					if (target_visible && best_visibility > 0.0f && dist <= (MELEE_RANGE * 1.5f))
					{
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_MELEE_BREATH);
						cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_ATTACK);
						cmd.forwardmove = 127;
					}
					else
					{
						if (target_visible && best_visibility >= 0.4f && dist > 256.0f)
						{
							cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_ADS);
						}

						const auto* const atk = sv_bots_press_attack_dvar();
						if (atk
							&& atk->current.enabled
							&& target_visible
							&& best_visibility > 0.0f
							&& dist <= FIRE_RANGE
							&& (aim_settled
								|| yaw_delta <= (FIRE_ALIGNMENT_THRESHOLD * 2)
								|| pitch_delta <= (FIRE_ALIGNMENT_THRESHOLD * 2)))
						{
							cmd.buttons = static_cast<game::usercmd_buttons>(cmd.buttons | game::BUTTON_ATTACK);

							if (dist > 224.0f)
							{
								cmd.forwardmove = std::max<std::int8_t>(cmd.forwardmove, 48);
								if (!stuck && ((behavior_tick + bot_idx) & 1) == 0)
								{
									cmd.rightmove = 32;
								}
								else if (!stuck)
								{
									cmd.rightmove = -32;
								}
							}
						}
						else if (state_idx >= 0)
						{
							s_attack_phase[state_idx] = false;
						}
					}
				}
				else
				{
					clear_bot_target_state(bot_idx);
					if (bot_ps)
					{
						apply_search_movement(cmd, bot_idx, bot_ps->commandTime);
					}
				}
			}

			*reinterpret_cast<int*>(client + 0x08) = *reinterpret_cast<int*>(client + 0x10) - 1;
			call_sv_client_think_real(client, &cmd);
			sanitize_bot_health(bot_idx, false, true);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			s_client_think_real_addr = reinterpret_cast<std::uintptr_t>(game::SV_ClientThink.get());

			utils::hook::call(game::game_offset(0x10414440), client_enter_world_hook);
			utils::hook::jump(reinterpret_cast<std::uintptr_t>(game::SV_BotUserMove.get()), sv_bot_user_move_hook);

			std::fill_n(s_tracked_target, MAX_CLIENTS, -1);

			scheduler::on_shutdown([]
				{
					std::memset(s_attack_phase, 0, sizeof(s_attack_phase));
					std::fill_n(s_tracked_target, MAX_CLIENTS, -1);
					std::memset(s_last_visible_time, 0, sizeof(s_last_visible_time));
					std::memset(s_last_seen_origin, 0, sizeof(s_last_seen_origin));
					std::memset(s_last_cmd_yaw, 0, sizeof(s_last_cmd_yaw));
					std::memset(s_last_cmd_pitch, 0, sizeof(s_last_cmd_pitch));
					std::memset(s_search_yaw, 0, sizeof(s_search_yaw));
					std::memset(s_next_search_time, 0, sizeof(s_next_search_time));
					std::memset(s_last_progress_time, 0, sizeof(s_last_progress_time));
					std::memset(s_last_progress_origin, 0, sizeof(s_last_progress_origin));
					s_last_applied_bot_max_health = 0;
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

			const auto botthink_site = reinterpret_cast<std::uintptr_t>(game::SV_BotUserMove.get());
			utils::hook::set<std::uint8_t>(botthink_site + 0, 0x83);
			utils::hook::set<std::uint8_t>(botthink_site + 1, 0xEC);
			utils::hook::set<std::uint8_t>(botthink_site + 2, 0x30);
			utils::hook::set<std::uint8_t>(botthink_site + 3, 0x83);
			utils::hook::set<std::uint8_t>(botthink_site + 4, 0xBE);
		}
	};
}

REGISTER_COMPONENT(bots::component)
