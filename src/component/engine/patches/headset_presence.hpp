#pragma once
#include <Windows.h>
#include <cstddef>

namespace voice_compatibility
{
	// Partial binding, not a voice implementation. GFWL SDK include/xhv.h:
	// IXHVEngine has no QueryInterface slot; IsHeadsetPresent is slot 13.
	// QoS 10323904/1032390E uses this exact stdcall (this, user-index) ABI.
	struct engine_view
	{
		void** vtable;
	};
	static_assert(offsetof(engine_view, vtable) == 0);
	static_assert(sizeof(engine_view) == 4, "QoS voice binding requires x86");

	inline BOOL WINAPI headset_present(void* engine, DWORD user_index)
	{
		if (!engine) return FALSE;
		const auto* view = static_cast<const engine_view*>(engine);
		using query = BOOL(WINAPI*)(void*, DWORD);
		return reinterpret_cast<query>(view->vtable[13])(engine, user_index);
	}
}
