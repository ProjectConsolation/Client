#pragma once

#include <algorithm>

namespace renderer_quality
{
	// D3D9 sample types 2..16 are sample counts; 1 is NONMASKABLE, not 1x MSAA.
	// Query returns zero for a failed check or an empty quality-level domain.
	template <typename Query>
	int select_extended_samples(const int requested, Query query)
	{
		for (int samples = std::clamp(requested, 1, 16); samples >= 2; --samples)
		{
			if (query(samples, false) && query(samples, true)) return samples;
		}
		return 0; // D3DMULTISAMPLE_NONE
	}
}
