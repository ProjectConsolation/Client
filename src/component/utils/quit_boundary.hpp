#pragma once

#include <atomic>

namespace scheduler
{
	// A quit command may run inside UI painting. It must not destroy engine
	// state until that frame has unwound to the outer main-loop boundary.
	class quit_boundary
	{
	public:
		void request()
		{
			auto expected = state::running;
			state_.compare_exchange_strong(expected, state::requested);
		}

		bool begin_quit()
		{
			auto expected = state::requested;
			return state_.compare_exchange_strong(expected, state::quitting);
		}

		bool stopped() const { return state_.load() == state::quitting; }

	private:
		enum class state { running, requested, quitting };
		std::atomic<state> state_{state::running};
	};
}
