#pragma once

namespace traversal_camera_policy
{
	// QoS traversal enum: 10556324 / playerState +3956. Mode 1 can be a
	// native body-camera write (1030D4BC); mode 2 is the explicit chase view.
	inline bool first_person(int pm_type, unsigned int pm_flags, int camera_mode,
		int traversal, bool mantle, bool climb)
	{
		if (pm_type >= 6 || !(pm_flags & 8u) || camera_mode == 2) return false;
		if (traversal >= 4 && traversal <= 6) return mantle;
		if (traversal == 2 || traversal == 3 || traversal == 7) return climb;
		return false;
	}
}
