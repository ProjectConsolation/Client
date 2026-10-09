#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace renderer_performance
{
	struct frame_sample
	{
		double interval_ms{};
		double swap_ms{};
		double present_ms{};
	};

	struct capture_buffer
	{
		static constexpr std::size_t capacity = 4096;
		std::array<frame_sample, capacity> samples{};
		std::size_t count{};
		std::size_t failures{};
		double previous_end_ms{};

		void record(const double end_ms, const double swap_ms, const double present_ms, const bool failed)
		{
			if (failed)
			{
				++failures;
				previous_end_ms = 0;
				return;
			}
			if (count == capacity || !std::isfinite(end_ms) || !std::isfinite(swap_ms)
				|| !std::isfinite(present_ms) || swap_ms < 0 || present_ms < 0) return;
			const double gap = end_ms - previous_end_ms;
			// Initial sample, debugger pauses, map loads and clock discontinuities
			// do not enter the cadence distribution. Swap timings remain visible.
			const double interval = previous_end_ms > 0 && gap > 0 && gap <= 250 ? gap : 0;
			samples[count++] = {interval, swap_ms, present_ms};
			previous_end_ms = end_ms;
		}
	};

	struct timing_summary
	{
		std::size_t count{};
		double mean{}, p50{}, p95{}, p99{}, maximum{};
	};

	inline timing_summary summarize(std::vector<double> values)
	{
		std::erase_if(values, [](double v) { return !std::isfinite(v) || v < 0; });
		if (values.empty()) return {};
		std::sort(values.begin(), values.end());
		double sum{};
		for (const auto value : values) sum += value;
		const auto percentile = [&values](const double fraction)
		{ return values[static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1]; };
		return {values.size(), sum / values.size(), percentile(0.50), percentile(0.95), percentile(0.99), values.back()};
	}
}
