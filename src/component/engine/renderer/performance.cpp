#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>
#include <d3d9.h>
#include <charconv>
#include <string_view>
#include "frame_timing.hpp"

namespace renderer_performance
{
	namespace
	{
		using clock = std::chrono::steady_clock;
		std::atomic_bool capturing{false};
		std::mutex capture_mutex;
		capture_buffer capture;
		unsigned generation{}; // protected by capture_mutex
		std::size_t target_frames{600};
		bool installed{};
		int (__cdecl* native_swap)(){};
		constexpr std::array<unsigned char, 5> swap_original{0xE8, 0x2D, 0xF1, 0xFF, 0xFF};
		constexpr std::array<unsigned char, 5> present_original{0x8B, 0x42, 0x0C, 0xFF, 0xD0};
		std::array<unsigned char, 5> swap_patch{}, present_patch{};
		struct phase_context
		{
			bool measuring{};
			double present_ms{};
			bool failed{};
		};
		thread_local phase_context phase;

		double milliseconds(const clock::duration duration)
		{
			return std::chrono::duration<double, std::milli>(duration).count();
		}

		HRESULT __stdcall present_stub(IDirect3DSwapChain9* const chain, const RECT* const source,
			const RECT* const destination, const HWND window, const RGNDATA* const dirty, const DWORD flags)
		{
			// QoS PC 103B3861: mov eax,[edx+0Ch]; call eax. Six stdcall
			// stack arguments (including this), matching SwapChain9::Present.
			// No vtable mutation or retained COM objects; reset ownership stays native.
			if (!phase.measuring) return chain->Present(source, destination, window, dirty, flags);
			const auto begin = clock::now();
			const auto result = chain->Present(source, destination, window, dirty, flags);
			phase.present_ms += milliseconds(clock::now() - begin);
			phase.failed |= FAILED(result);
			return result; // preserve native device-loss/error handling
		}

		int __cdecl swap_stub()
		{
			if (!capturing.load(std::memory_order_relaxed)) return native_swap();
			unsigned token;
			bool enabled;
			{
				std::lock_guard lock(capture_mutex);
				enabled = capturing.load(std::memory_order_relaxed);
				token = generation;
			}
			if (!enabled) return native_swap();
			phase = {true, 0, false};
			const auto begin = clock::now();
			const auto result = native_swap();
			const auto end = clock::now();
			phase.measuring = false;
			{
				std::lock_guard lock(capture_mutex);
				// A stop/restart command may have arrived during native Present.
				if (capturing.load(std::memory_order_relaxed) && generation == token)
				{
					capture.record(milliseconds(end.time_since_epoch()), milliseconds(end - begin), phase.present_ms, phase.failed);
					if (capture.count >= target_frames || capture.failures >= target_frames) capturing.store(false);
				}
			}
			return result;
		}

		void print_summary(const char* const label, const std::vector<double>& values)
		{
			const auto summary = summarize(values);
			console::info("gfxperf: %s n=%u mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f ms\n",
				label, static_cast<unsigned>(summary.count), summary.mean, summary.p50,
				summary.p95, summary.p99, summary.maximum);
		}

		void report()
		{
			std::vector<frame_sample> samples;
			std::size_t failures;
			{
				std::lock_guard lock(capture_mutex);
				capturing.store(false);
				++generation;
				samples.assign(capture.samples.begin(), capture.samples.begin() + capture.count);
				failures = capture.failures;
			}
			console::info("gfxperf: stopped; successful swaps=%u failed presents=%u (CPU wall-clock timings, NOT GPU time)\n",
				static_cast<unsigned>(samples.size()), static_cast<unsigned>(failures));
			if (samples.empty()) return;
			std::vector<double> cadence, swaps, presents, outside_present;
			for (const auto& sample : samples)
			{
				if (sample.interval_ms > 0) cadence.push_back(sample.interval_ms);
				swaps.push_back(sample.swap_ms);
				presents.push_back(sample.present_ms);
				outside_present.push_back(std::max(0.0, sample.swap_ms - sample.present_ms));
			}
			print_summary("swap-end interval", cadence);
			print_summary("native swap phase", swaps);
			print_summary("inside Present", presents);
			print_summary("swap outside Present", outside_present);
			const auto* cap = game::Dvar_FindVar("com_maxfps");
			const auto* vsync = game::Dvar_FindVar("r_vsync");
			const auto* aa = game::Dvar_FindVar("r_aaSamples");
			console::info("gfxperf: report-time settings com_maxfps=%d r_vsync=%d r_aaSamples=%d; keep settings/focus fixed during comparisons\n",
				cap ? cap->current.integer : -1, vsync ? vsync->current.enabled : -1, aa ? aa->current.integer : -1);
		}

		void performance_command(const command::params& params)
		{
			const std::string_view action = params.size() > 1 ? params[1] : "report";
			if ((action == "report" || action == "stop") && params.size() <= 2) { report(); return; }
			if (action != "start" || params.size() > 3)
			{
				console::info("usage: gfxperf start [frames: 30..4096] | stop | report\n");
				return;
			}
			if (!installed) { console::warn("gfxperf: hooks unavailable for this build\n"); return; }
			unsigned frames = 600;
			if (params.size() == 3)
			{
				const std::string_view text = params[2];
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), frames);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || frames < 30 || frames > capture_buffer::capacity)
				{
					console::warn("gfxperf: frames must be an integer from 30 to 4096\n");
					return;
				}
			}
			{
				std::lock_guard lock(capture_mutex);
				capturing.store(false);
				++generation;
				capture.count = capture.failures = 0;
				capture.previous_end_ms = 0;
				target_frames = frames;
				capturing.store(true);
			}
			console::info("gfxperf: recording up to %u successful swaps; auto-stops, then use gfxperf report\n", frames);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// Compared with KisakCOD rb_backend.cpp RB_SwapBuffers. QoS adds
			// a focus/critical-section Sleep(16) before Present (103B383D), so
			// measure both boundaries rather than mislabeling that wait as GPU work.
			const auto swap = game::game_offset(0x103B46DE);
			const auto present = game::game_offset(0x103B3861);
			if (std::memcmp(reinterpret_cast<const void*>(swap), swap_original.data(), 5) == 0
				&& std::memcmp(reinterpret_cast<const void*>(present), present_original.data(), 5) == 0)
			{
				native_swap = reinterpret_cast<int (__cdecl*)()>(game::game_offset(0x103B3810));
				// Validate both relative branches before modifying either site.
				if (!utils::hook::is_relatively_far(reinterpret_cast<void*>(swap), reinterpret_cast<void*>(swap_stub))
					&& !utils::hook::is_relatively_far(reinterpret_cast<void*>(present), reinterpret_cast<void*>(present_stub)))
				{
					utils::hook::call(present, present_stub);
					utils::hook::call(swap, swap_stub);
					std::memcpy(swap_patch.data(), reinterpret_cast<const void*>(swap), 5);
					std::memcpy(present_patch.data(), reinterpret_cast<const void*>(present), 5);
					installed = true;
				}
			}
			if (!installed) console::warn("[renderer] gfxperf skipped: native hook sites unavailable\n");
			scheduler::once([] { command::add("gfxperf", performance_command); }, scheduler::main);
		}

		void pre_destroy() override
		{
			capturing.store(false);
			if (!installed) return;
			const auto restore = [](const std::uintptr_t address, const auto& patch, const auto& original)
			{
				const auto site = game::game_offset(address);
				if (std::memcmp(reinterpret_cast<const void*>(site), patch.data(), patch.size()) == 0)
					utils::hook::set(site, original);
			};
			restore(0x103B46DE, swap_patch, swap_original);
			restore(0x103B3861, present_patch, present_original);
			installed = false;
		}
	};
}

REGISTER_COMPONENT(renderer_performance::component)
