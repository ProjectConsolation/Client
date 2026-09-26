#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/game_console.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/string.hpp>

#include <algorithm>
#include <cstring>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace draw_origin
{
	namespace
	{
		constexpr auto cg_initialized_address = 0x129FE8E4;
		constexpr auto player_origin_address = 0x12A4CE1C;
		constexpr auto player_velocity_address = 0x12A4CE28;
		constexpr float native_line_spacing = 0.75f;
		constexpr float native_text_y_scale = 1.1f;
		constexpr float memory_text_scale = 1.2f;
		constexpr float memory_line_spacing = 0.9f;
		constexpr float right_margin = 4.0f;
		constexpr auto console_update_interval = 500ms;
		constexpr auto memory_update_interval = 250ms;
		constexpr auto address_space_update_interval = 1s;
		constexpr float overlay_top_margin = 6.0f;
		// JB_LiveEngine_s.exe is a 32-bit image without LARGEADDRESSAWARE.
		constexpr std::uint64_t process_address_space_limit = 2ull * 1024ull * 1024ull * 1024ull;
		constexpr double memory_warning_fraction = 0.75;
		constexpr double memory_critical_fraction = 0.90;

		float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float warning_color[4] = {1.0f, 0.82f, 0.15f, 1.0f};
		float critical_color[4] = {1.0f, 0.18f, 0.12f, 1.0f};
		float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.75f};
		struct memory_snapshot
		{
			SIZE_T working_set{};
			SIZE_T private_bytes{};
			SIZE_T peak_working_set{};
			std::uint64_t free_address_space{};
			bool valid{};
		};
		struct memory_report_line
		{
			std::string text;
			const float* color = text_color;
		};

		memory_snapshot process_memory{};
		std::vector<memory_report_line> console_memory_report;
		bool console_memory_report_valid = false;
		std::chrono::steady_clock::time_point next_memory_update{};
		std::chrono::steady_clock::time_point next_address_space_update{};
		std::chrono::steady_clock::time_point next_console_memory_update{};
#ifdef DEBUG
		std::atomic_bool internal_console_status_enabled = false;
#endif

		bool read_player_motion(float (&origin)[3], float (&velocity)[3])
		{
			__try
			{
				if (!*reinterpret_cast<std::uintptr_t*>(game::game_offset(cg_initialized_address)))
				{
					return false;
				}

				const auto* const source_origin = reinterpret_cast<const float*>(game::game_offset(player_origin_address));
				const auto* const source_velocity = reinterpret_cast<const float*>(game::game_offset(player_velocity_address));
				for (auto index = 0; index < 3; ++index)
				{
					origin[index] = source_origin[index];
					velocity[index] = source_velocity[index];
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void draw_right_aligned(const char* text, game::Font_s* font, const float right,
			const float y, const float scale = 1.0f, const float* color = text_color)
		{
			const auto width = static_cast<float>(game::R_TextWidth(text, 0x7FFFFFFF, font)) * scale;
			const auto x = right - width - right_margin;
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x + 1.0f, y + 1.0f,
				scale, scale * native_text_y_scale, 0.0f, shadow_color, 0);
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x, y,
				scale, scale * native_text_y_scale, 0.0f, color, 0);
		}

		int fps_line_count()
		{
			const auto* const draw_fps = game::Dvar_FindVar("cg_drawFPS");
			if (!draw_fps || draw_fps->current.integer <= 0)
			{
				return 0;
			}

			return draw_fps->current.integer == 1 ? 1 : 3;
		}

		float get_client_width()
		{
			RECT client_rect{};
			const auto window = *game::main_window;
			if (window && GetClientRect(window, &client_rect) && client_rect.right > client_rect.left)
			{
				return static_cast<float>(client_rect.right - client_rect.left);
			}

			return 640.0f;
		}

		float get_client_height()
		{
			RECT client_rect{};
			const auto window = *game::main_window;
			if (window && GetClientRect(window, &client_rect) && client_rect.bottom > client_rect.top)
			{
				return static_cast<float>(client_rect.bottom - client_rect.top);
			}

			return 480.0f;
		}

		void update_process_memory()
		{
			const auto now = std::chrono::steady_clock::now();
			if (now < next_memory_update)
			{
				return;
			}

			next_memory_update = now + memory_update_interval;
			if (now >= next_address_space_update)
			{
				next_address_space_update = now + address_space_update_interval;
				constexpr std::uintptr_t minimum_application_address = 0x10000;
				constexpr std::uintptr_t maximum_application_address = 0x7FFF0000;
				std::uint64_t free_bytes = 0;
				auto address = minimum_application_address;
				while (address < maximum_application_address)
				{
					MEMORY_BASIC_INFORMATION region{};
					if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)))
					{
						break;
					}

					const auto region_start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
					const auto region_end = region_start + region.RegionSize;
					if (region.State == MEM_FREE)
					{
						const auto clipped_start = (std::max<std::uintptr_t>)(
							region_start, minimum_application_address);
						const auto clipped_end = (std::min<std::uintptr_t>)(
							static_cast<std::uintptr_t>(region_end), maximum_application_address);
						if (clipped_end > clipped_start)
						{
							free_bytes += clipped_end - clipped_start;
						}
					}

					if (region_end <= address)
					{
						break;
					}

					address = region_end;
				}

				process_memory.free_address_space = free_bytes;
			}

			PROCESS_MEMORY_COUNTERS_EX counters{};
			counters.cb = sizeof(counters);
			if (GetProcessMemoryInfo(GetCurrentProcess(),
				reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
			{
				process_memory.working_set = counters.WorkingSetSize;
				process_memory.private_bytes = counters.PrivateUsage;
				process_memory.peak_working_set = counters.PeakWorkingSetSize;
				process_memory.valid = true;
			}
			else
			{
				process_memory.valid = false;
			}
		}

		std::string format_grouped_bytes(const std::uint64_t bytes)
		{
			auto value = std::to_string(bytes);
			for (auto position = static_cast<std::ptrdiff_t>(value.size()) - 3;
				position > 0; position -= 3)
			{
				value.insert(static_cast<std::size_t>(position), 1, ',');
			}

			return value;
		}

		std::string format_memory(const char* label, const SIZE_T bytes, const int mode)
		{
			constexpr auto bytes_per_megabyte = 1024.0 * 1024.0;
			if (mode >= 2)
			{
				return utils::string::va("%s: %s bytes / %s bytes", label,
					format_grouped_bytes(bytes).c_str(),
					format_grouped_bytes(process_address_space_limit).c_str());
			}

			return utils::string::va("%s: %.1f MB / %.0f MB", label,
				static_cast<double>(bytes) / bytes_per_megabyte,
				static_cast<double>(process_address_space_limit) / bytes_per_megabyte);
		}

		std::string format_free_memory(const std::uint64_t bytes, const int mode)
		{
			constexpr auto bytes_per_megabyte = 1024.0 * 1024.0;
			if (mode >= 2)
			{
				return utils::string::va("free: %s bytes",
					format_grouped_bytes(bytes).c_str());
			}

			return utils::string::va("free: %.1f MB",
				static_cast<double>(bytes) / bytes_per_megabyte);
		}

		bool guarded_read(const void* const source, void* const destination, const std::size_t size)
		{
			__try
			{
				std::memcpy(destination, source, size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		template <typename T>
		bool read_game_value(const std::uintptr_t ida_address, T& value)
		{
			return guarded_read(reinterpret_cast<const void*>(game::game_offset(ida_address)),
				&value, sizeof(value));
		}

		const float* memory_color(SIZE_T bytes);

		std::string format_hunk_value(const char* label, const std::int32_t value, const int mode)
		{
			if (mode == 2)
			{
				return utils::string::va("%8i %s", value, label);
			}

			const auto magnitude = value < 0
				? static_cast<std::uint64_t>(-static_cast<std::int64_t>(value))
				: static_cast<std::uint64_t>(value);
			const auto is_byte_total = std::strcmp(label, "bytes total hunk") == 0;
			return utils::string::va("%s%s%s%s", value < 0 ? "-" : "",
				format_grouped_bytes(magnitude).c_str(), is_byte_total ? " " : " bytes ", label);
		}

		std::string format_memory_size(const char* label, const std::int64_t bytes, const int mode)
		{
			constexpr auto bytes_per_megabyte = 1024.0 * 1024.0;
			if (mode == 3)
			{
				const auto magnitude = bytes < 0
					? static_cast<std::uint64_t>(-bytes)
					: static_cast<std::uint64_t>(bytes);
				return utils::string::va("%s %s%s bytes", label,
					bytes < 0 ? "-" : "", format_grouped_bytes(magnitude).c_str());
			}

			return utils::string::va("%s %5.1f", label,
				static_cast<double>(bytes) / bytes_per_megabyte);
		}

		bool read_zone_name(const std::uintptr_t address, char (&name)[15])
		{
			std::memcpy(name, "(null)", 7);
			name[14] = '\0';
			if (!address)
			{
				return true;
			}

			char source[14]{};
			if (!guarded_read(reinterpret_cast<const void*>(address), source, sizeof(source)))
			{
				return false;
			}

			std::memcpy(name, source, sizeof(source));
			name[14] = '\0';
			return true;
		}

		void update_console_memory_report(const int mode)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now < next_console_memory_update)
			{
				return;
			}

			next_console_memory_update = now + memory_update_interval;
			console_memory_report.clear();
			console_memory_report_valid = false;

			// QoS PC 1.1 meminfo callback (0x1026F3C0) and its physical/zone
			// helper (0x1026EA80), traced from their native format-string xrefs.
			std::int32_t total_hunk_bytes{};
			std::int32_t low_permanent{};
			std::int32_t low_temporary{};
			std::int32_t high_permanent{};
			std::int32_t high_temporary{};
			if (!read_game_value(0x114EBBB0, total_hunk_bytes)
				|| !read_game_value(0x1156BBC4, low_permanent)
				|| !read_game_value(0x1156BBC8, low_temporary)
				|| !read_game_value(0x1156BBBC, high_permanent)
				|| !read_game_value(0x1156BBC0, high_temporary))
			{
				return;
			}

			const auto report_color = memory_color(process_memory.private_bytes);
			const auto add_line = [&](std::string text, const float* color = nullptr)
			{
				console_memory_report.push_back({std::move(text), color ? color : report_color});
			};
			const auto add_blank = [&] { console_memory_report.push_back({}); };

			add_line(format_hunk_value("bytes total hunk", total_hunk_bytes, mode), report_color);
			add_blank();
			add_line(format_hunk_value("low permanent", low_permanent, mode));
			if (low_temporary != low_permanent)
			{
				add_line(format_hunk_value("low temp", low_temporary, mode));
			}
			add_blank();
			add_line(format_hunk_value("high permanent", high_permanent, mode));
			if (high_temporary != high_permanent)
			{
				add_line(format_hunk_value("high temp", high_temporary, mode));
			}
			add_blank();
			add_line(format_hunk_value("total hunk in use",
				low_permanent + high_permanent, mode));

			std::int32_t physical_base{};
			std::int32_t physical_end{};
			std::int32_t executable_bytes{};
			if (!read_game_value(0x1156CBDC, physical_base)
				|| !read_game_value(0x1156CCE8, physical_end)
				|| !read_game_value(0x11A76564, executable_bytes))
			{
				return;
			}

			const auto physical_free_bytes = static_cast<std::int32_t>(
				static_cast<std::uint32_t>(physical_end) - static_cast<std::uint32_t>(physical_base));
			constexpr std::int64_t physical_warning_bytes = 256ll * 1024ll * 1024ll;
			constexpr std::int64_t physical_critical_bytes = 128ll * 1024ll * 1024ll;
			const auto* physical_color = physical_free_bytes < physical_critical_bytes
				? critical_color
				: (physical_free_bytes < physical_warning_bytes ? warning_color : report_color);
			add_line(format_memory_size("free physical", physical_free_bytes, mode), physical_color);
			add_line(format_memory_size("exe", executable_bytes, mode));

			std::uint32_t high_zone_count{};
			if (!read_game_value(0x1156CCE4, high_zone_count) || high_zone_count > 64)
			{
				return;
			}

			for (std::uint32_t index = 0; index < high_zone_count; ++index)
			{
				std::uintptr_t name_address{};
				std::int32_t zone_end{};
				std::int32_t zone_start{};
				const auto name_slot = 0x1156CCEC + index * 8;
				const auto end_slot = 0x1156CCF0 + index * 8;
				if (!read_game_value(name_slot, name_address)
					|| !read_game_value(end_slot, zone_end)
					|| !(index + 1 == high_zone_count
						? read_game_value(0x1156CCE8, zone_start)
						: read_game_value(0x1156CCF8 + index * 8, zone_start)))
				{
					return;
				}

				char name[15]{};
				if (!read_zone_name(name_address, name))
				{
					return;
				}

				const auto zone_bytes = static_cast<std::int32_t>(
					static_cast<std::uint32_t>(zone_end) - static_cast<std::uint32_t>(zone_start));
				if (mode == 2)
				{
					add_line(utils::string::va("(hi)%-14.14s %5.1f", name,
						static_cast<double>(zone_bytes) / (1024.0 * 1024.0)));
				}
				else
				{
					const auto magnitude = zone_bytes < 0
						? static_cast<std::uint64_t>(-static_cast<std::int64_t>(zone_bytes))
						: static_cast<std::uint64_t>(zone_bytes);
					add_line(utils::string::va("(hi)%-14.14s %s bytes", name,
						format_grouped_bytes(magnitude).c_str()));
				}
			}

			std::uint32_t low_zone_count{};
			std::int32_t low_zone_end{};
			if (!read_game_value(0x1156CBD8, low_zone_count) || low_zone_count > 64
				|| !read_game_value(0x1156CBDC, low_zone_end))
			{
				return;
			}

			for (auto index = static_cast<std::int32_t>(low_zone_count) - 1; index >= 0; --index)
			{
				std::uintptr_t name_address{};
				std::int32_t zone_start{};
				const auto name_slot = 0x1156CBE0 + static_cast<std::uint32_t>(index) * 8;
				const auto start_slot = 0x1156CBE4 + static_cast<std::uint32_t>(index) * 8;
				if (!read_game_value(name_slot, name_address) || !read_game_value(start_slot, zone_start))
				{
					return;
				}

				char name[15]{};
				if (!read_zone_name(name_address, name))
				{
					return;
				}

				const auto zone_bytes = static_cast<std::int32_t>(
					static_cast<std::uint32_t>(low_zone_end) - static_cast<std::uint32_t>(zone_start));
				if (mode == 2)
				{
					add_line(utils::string::va("(lo)%-14.14s %5.1f", name,
						static_cast<double>(zone_bytes) / (1024.0 * 1024.0)));
				}
				else
				{
					const auto magnitude = zone_bytes < 0
						? static_cast<std::uint64_t>(-static_cast<std::int64_t>(zone_bytes))
						: static_cast<std::uint64_t>(zone_bytes);
					add_line(utils::string::va("(lo)%-14.14s %s bytes", name,
						format_grouped_bytes(magnitude).c_str()));
				}
				low_zone_end = zone_start;
			}

			add_line("------------------------");
			console_memory_report_valid = true;
		}

		const float* memory_color(const SIZE_T bytes)
		{
			const auto fraction = static_cast<double>(bytes)
				/ static_cast<double>(process_address_space_limit);
			if (fraction >= memory_critical_fraction)
			{
				return critical_color;
			}
			if (fraction >= memory_warning_fraction)
			{
				return warning_color;
			}

			return text_color;
		}

		const char* format_motion(const float (&origin)[3], const float (&velocity)[3])
		{
			return utils::string::va(
				"origin: (%.2f, %.2f, %.2f) velocity: (%.2f, %.2f, %.2f)",
				origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2]);
		}

#ifdef DEBUG
		void update_internal_console_status()
		{
			if (!internal_console_status_enabled.load(std::memory_order_relaxed))
			{
				return;
			}

			float origin[3]{};
			float velocity[3]{};
			if (read_player_motion(origin, velocity))
			{
				game_console::set_live_status(format_motion(origin, velocity));
			}
			else
			{
				game_console::set_live_status("origin: unavailable velocity: unavailable");
			}
		}

		void origin_command(const command::params& params)
		{
			bool enabled = !internal_console_status_enabled.load(std::memory_order_relaxed);
			if (params.size() > 2)
			{
				game_console::set_live_status("usage: origin [0|1]");
				return;
			}

			if (params.size() == 2)
			{
				if (!_stricmp(params[1], "1") || !_stricmp(params[1], "on"))
				{
					enabled = true;
				}
				else if (!_stricmp(params[1], "0") || !_stricmp(params[1], "off"))
				{
					enabled = false;
				}
				else
				{
					game_console::set_live_status("usage: origin [0|1]");
					return;
				}
			}

			internal_console_status_enabled.store(enabled, std::memory_order_relaxed);
			if (enabled)
			{
				update_internal_console_status();
			}
			else
			{
				game_console::clear_live_status();
			}
		}
#endif

		void draw()
		{
			const auto draw_origin = dvars::cg_drawOrigin && dvars::cg_drawOrigin->current.enabled;
			const auto memory_mode = dvars::cg_drawMemInfo ? dvars::cg_drawMemInfo->current.integer : 0;
			const auto draw_memory = memory_mode > 0;
			if (!draw_origin && !draw_memory)
			{
				return;
			}

			auto* const font = game::R_RegisterFont("fonts/consolefont");
			if (!font || font->pixelHeight <= 0)
			{
				return;
			}

			// The local ScreenPlacement declaration does not match this QoS build.
			// Direct renderer text coordinates are client pixels, as confirmed by the
			// version overlay, so keep this overlay in the actual window bounds too.
			const auto right = get_client_width();
			float y = overlay_top_margin;
			const auto line_height = static_cast<float>(font->pixelHeight) * native_line_spacing;

			if (draw_memory)
			{
				update_process_memory();
				const auto memory_line_height = static_cast<float>(font->pixelHeight)
					* memory_text_scale * memory_line_spacing;
				if (memory_mode == 1 && process_memory.valid)
				{
					const auto block_height = memory_line_height * 4.5f;
					float memory_y = (get_client_height() - block_height) * 0.5f
						+ static_cast<float>(font->pixelHeight) * memory_text_scale;

					auto text = format_memory("working", process_memory.working_set, memory_mode);
					draw_right_aligned(text.c_str(), font, right, memory_y, memory_text_scale,
						memory_color(process_memory.working_set));

					memory_y += memory_line_height;
					text = format_memory("private", process_memory.private_bytes, memory_mode);
					draw_right_aligned(text.c_str(), font, right, memory_y, memory_text_scale,
						memory_color(process_memory.private_bytes));

					memory_y += memory_line_height;
					text = format_memory("peak", process_memory.peak_working_set, memory_mode);
					draw_right_aligned(text.c_str(), font, right, memory_y, memory_text_scale,
						memory_color(process_memory.peak_working_set));

					memory_y += memory_line_height * 1.5f;
					text = format_free_memory(process_memory.free_address_space, memory_mode);
					const auto used_fraction = 1.0 - static_cast<double>(process_memory.free_address_space)
						/ static_cast<double>(process_address_space_limit);
					const auto* free_color = used_fraction >= memory_critical_fraction
						? critical_color
						: (used_fraction >= memory_warning_fraction ? warning_color : text_color);
					draw_right_aligned(text.c_str(), font, right, memory_y, memory_text_scale,
						free_color);
				}
				else if (memory_mode >= 2)
				{
					update_console_memory_report(memory_mode);
					const auto line_count = console_memory_report_valid
						? console_memory_report.size()
						: 1;
					const auto blank_count = console_memory_report_valid
						? std::count_if(console_memory_report.begin(), console_memory_report.end(),
							[](const memory_report_line& line) { return line.text.empty(); })
						: 0;
					const auto block_height = memory_line_height
						* (static_cast<float>(line_count) + static_cast<float>(blank_count) * 0.5f);
					float memory_y = (get_client_height() - block_height) * 0.5f
						+ static_cast<float>(font->pixelHeight) * memory_text_scale;

					if (!console_memory_report_valid)
					{
						draw_right_aligned("meminfo: unavailable", font, right, memory_y,
							memory_text_scale, critical_color);
					}
					else
					{
						for (const auto& line : console_memory_report)
						{
							if (line.text.empty())
							{
								memory_y += memory_line_height * 1.5f;
								continue;
							}

							draw_right_aligned(line.text.c_str(), font, right, memory_y,
								memory_text_scale, line.color);
							memory_y += memory_line_height;
						}
					}
				}
			}

			if (!draw_origin)
			{
				return;
			}

			float origin[3]{};
			float velocity[3]{};
			if (!read_player_motion(origin, velocity))
			{
				return;
			}

			y += line_height * static_cast<float>(fps_line_count());
			y += static_cast<float>(font->pixelHeight) * native_text_y_scale;
			const auto origin_text = utils::string::va("origin: (%.2f, %.2f, %.2f)", origin[0], origin[1], origin[2]);
			draw_right_aligned(origin_text, font, right, y);

			y += line_height;
			const auto velocity_text = utils::string::va("velocity: (%.2f, %.2f, %.2f)", velocity[0], velocity[1], velocity[2]);
			draw_right_aligned(velocity_text, font, right, y);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::loop(draw, scheduler::pipeline::renderer);
#ifdef DEBUG
			command::add("origin", origin_command);
			scheduler::loop(update_internal_console_status, scheduler::pipeline::main, console_update_interval);
#endif
		}

		void pre_destroy() override
		{
#ifdef DEBUG
			internal_console_status_enabled.store(false, std::memory_order_relaxed);
			game_console::clear_live_status();
#endif
		}
	};
}

REGISTER_COMPONENT(draw_origin::component)
