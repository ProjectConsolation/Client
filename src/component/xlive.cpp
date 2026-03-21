#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>

// ============================================================================
// xlive.cpp
//
// Bypasses GFWL/xlive anti-debug.
//
// 1. xlive_5030 = XLivePreTranslateMessage (0x104765F0)
//    Called from the main message pump (sub_10244EC0) for every Win32
//    message.  When xlive detects a debugger it starts consuming messages
//    (returns non-zero), preventing input from reaching the game.
//    Fix: stub to always return 0 (message not consumed).
//
// 2. xlive_5002 = XLiveInitialize (called from sub_103B4670)
//    Triggers the GFWL login overlay and registers xlive's internal
//    watchdog thread.  That watchdog fires if the main thread is paused
//    (i.e. at a breakpoint) for more than ~5 seconds, which causes xlive
//    to spam errors and *eventually* kill the process.
//    Fix: stub to return S_OK (0) without initializing GFWL.
//
// 3. xlive_5001 = called from WndProc (sub_102C4340) on every frame.
//    This is XLiveSetDebugLevel or similar -- when a debugger is present
//    xlive uses this to ratchet up its error logging and error dialogs.
//    Fix: stub to return S_OK.
//
// 4. xlive_5007 = called after xlive_5001, likely XLiveUpdate heartbeat.
//    Fix: stub to return S_OK.
// ============================================================================

namespace xlive
{
	namespace
	{
		int __stdcall xlive_5030_stub(void* /*pMsg*/)
		{
			// XLivePreTranslateMessage: return 0 = "message not consumed,
			// pass it along to TranslateMessage/DispatchMessage".
			return 0;
		}

		int __stdcall xlive_5002_stub()
		{
			// XLiveInitialize: return S_OK without touching GFWL.
			// This prevents the login overlay, watchdog thread, and
			// all the debug-detection code inside xlive from running.
			return 0;
		}

		int __stdcall xlive_5001_stub(void* /*pArg*/)
		{
			// XLiveSetDebugLevel / XLiveFrame: return S_OK.
			return 0;
		}

		int __stdcall xlive_5007_stub(void* /*pArg*/)
		{
			// XLiveUpdate heartbeat: return S_OK.
			return 0;
		}

	} // anonymous namespace

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
#ifdef DEBUG
			// Only apply these patches in debug builds.
			// In release they are unnecessary and you want xlive working
			// for online play.

			// XLivePreTranslateMessage -- stops input consumption under debugger
			utils::hook::jump(game::game_offset(0x10150A56), xlive_5030_stub); // xlive_5030 @ 0x10150A56

			// XLiveInitialize -- prevents watchdog thread + GFWL overlay
			utils::hook::jump(game::game_offset(0x10150A6E), xlive_5002_stub); // xlive_5002 @ 0x10150A6E

			// Per-frame xlive calls -- stop error spam
			utils::hook::jump(game::game_offset(0x10150AB6), xlive_5001_stub); // xlive_5001 @ 0x10150AB6
			utils::hook::jump(game::game_offset(0x10150A62), xlive_5007_stub); // xlive_5007 @ 0x10150A62

#endif
		}
	};

}

REGISTER_COMPONENT(xlive::component)