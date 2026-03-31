#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "console.hpp"
#include "filesystem.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/nt.hpp>

namespace music
{
	namespace
	{
		utils::hook::detour ail_set_stream_loop_block_hook;

		constexpr auto music_stream_callsite_1 = 0x103FFDC0;
		constexpr auto music_stream_callsite_2 = 0x103FFE5B;
		constexpr auto music_stream_callsite_3 = 0x104012BF;
		constexpr auto streamed_sound_open_function = 0x102C6F20;
		constexpr auto ail_open_stream_callsite_1 = 0x102C7082;
		constexpr auto ail_open_stream_callsite_2 = 0x102C7122;

		struct streamed_sound_file_view
		{
			const char* directory;
			const char* name;
		};

		std::string current_override_path;
		std::string last_notified_override;

		std::string sanitize_name(std::string value)
		{
			for (auto& ch : value)
			{
				switch (ch)
				{
				case '\\':
				case '/':
				case ':':
				case '*':
				case '?':
				case '"':
				case '<':
				case '>':
				case '|':
					ch = '_';
					break;
				default:
					break;
				}
			}

			return value;
		}

		void add_candidate(std::vector<std::string>& candidates, const std::string& value)
		{
			if (value.empty())
			{
				return;
			}

			if (std::find(candidates.begin(), candidates.end(), value) == candidates.end())
			{
				candidates.emplace_back(value);
			}
		}

		std::string find_music_override(const char*** request)
		{
			if (!request || !*request)
			{
				return {};
			}

			std::vector<std::string> candidates;

			const auto* const alias_name = (*request)[0];
			if (alias_name && *alias_name)
			{
				const auto safe_alias = sanitize_name(alias_name);
				add_candidate(candidates, std::format("sounds/music/{}.mp3", safe_alias));
				add_candidate(candidates, std::format("sounds/music/{}.wav", safe_alias));
			}

			const auto* const file_info = reinterpret_cast<const streamed_sound_file_view*>((*request)[4]);
			if (file_info && file_info->name && *file_info->name)
			{
				const auto file_name = std::filesystem::path(file_info->name);
				const auto filename = file_name.filename().generic_string();
				const auto stem = file_name.stem().generic_string();

				add_candidate(candidates, std::format("sounds/music/{}", filename));
				add_candidate(candidates, std::format("sounds/music/{}.mp3", stem));
				add_candidate(candidates, std::format("sounds/music/{}.wav", stem));
			}

			for (const auto& candidate : candidates)
			{
				std::string real_path;
				if (filesystem::find_file(candidate, &real_path))
				{
					return real_path;
				}
			}

			return {};
		}

		int __stdcall ail_open_stream_stub(int driver, const char* filename, int flags)
		{
			using ail_open_stream_t = int(__stdcall*)(int, const char*, int);
			static const auto original = utils::nt::library("mss32.dll").get_proc<ail_open_stream_t>("_AIL_open_stream@12");
			if (!original)
			{
				return 0;
			}

			game::Com_Printf(
				0,
				"sounds: AIL_open_stream file='%s' override='%s'\n",
				filename ? filename : "<null>",
				current_override_path.empty() ? "<none>" : current_override_path.c_str()
			);

			if (!current_override_path.empty())
			{
				filename = current_override_path.c_str();
			}

			return original(driver, filename, flags);
		}

		void __stdcall ail_set_stream_loop_block_stub(int stream, int loop_start, int loop_end)
		{
			if (!current_override_path.empty())
			{
				game::Com_Printf(
					0,
					"sounds: skipping stock loop block %d..%d for override='%s'\n",
					loop_start,
					loop_end,
					current_override_path.c_str()
				);
				return;
			}

			ail_set_stream_loop_block_hook.invoke<void>(stream, loop_start, loop_end);
		}

		void show_override_notice(const char* alias_name, const std::string& override_path)
		{
			if (override_path.empty())
			{
				return;
			}

			const auto alias = alias_name && *alias_name ? alias_name : "<unknown>";
			const auto key = std::format("{}|{}", alias, override_path);
			if (last_notified_override == key)
			{
				return;
			}

			last_notified_override = key;

			console::warn("overriding %s with %s\n", alias, override_path.c_str());
		}

		int __cdecl open_music_stream_stub(const char*** request, int index)
		{
			using open_stream_t = int(__cdecl*)(const char***, int);
			const auto original = reinterpret_cast<open_stream_t>(game::game_offset(streamed_sound_open_function));

			const char* alias_name = "<null>";
			const char* source_name = "<null>";
			if (request && *request)
			{
				if ((*request)[0] && *(*request)[0])
				{
					alias_name = (*request)[0];
				}

				const auto* const file_info = reinterpret_cast<const streamed_sound_file_view*>((*request)[4]);
				if (file_info && file_info->name && *file_info->name)
				{
					source_name = file_info->name;
				}
			}

			current_override_path = find_music_override(request);
			game::Com_Printf(
				0,
				"sounds: music stream alias='%s' source='%s' override='%s'\n",
				alias_name,
				source_name,
				current_override_path.empty() ? "<none>" : current_override_path.c_str()
			);

			show_override_notice(alias_name, current_override_path);

			const auto clear_override = gsl::finally([]()
			{
				current_override_path.clear();
			});

			return original(request, index);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			if (const auto proc = utils::nt::library("mss32.dll").get_proc<void*>("_AIL_set_stream_loop_block@12"))
			{
				ail_set_stream_loop_block_hook.create(proc, ail_set_stream_loop_block_stub);
			}

			utils::hook::call(game::game_offset(music_stream_callsite_1), open_music_stream_stub);
			utils::hook::call(game::game_offset(music_stream_callsite_2), open_music_stream_stub);
			utils::hook::call(game::game_offset(music_stream_callsite_3), open_music_stream_stub);
			utils::hook::call(game::game_offset(ail_open_stream_callsite_1), ail_open_stream_stub);
			utils::hook::call(game::game_offset(ail_open_stream_callsite_2), ail_open_stream_stub);
		}

		void pre_destroy() override
		{
			ail_set_stream_loop_block_hook.clear();
		}
	};
}


REGISTER_COMPONENT(music::component)
