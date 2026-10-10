#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "scheduler.hpp"
#include "quit_boundary.hpp"
#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/concurrency.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>

#include <array>
#include <cstring>

namespace scheduler
{
	namespace
	{
		struct task
		{
			std::function<bool()> handler{};
			std::chrono::milliseconds interval{};
			std::chrono::high_resolution_clock::time_point last_call{};
		};

		using task_list = std::vector<task>;

		class task_pipeline
		{
		public:
			void add(task&& task)
			{
				new_callbacks_.access([&task](task_list& tasks)
				{
					tasks.emplace_back(std::move(task));
				});
			}

			void execute()
			{
				callbacks_.access([&](task_list& tasks)
				{
					this->merge_callbacks();

					for (auto i = tasks.begin(); i != tasks.end();)
					{
						const auto now = std::chrono::high_resolution_clock::now();
						const auto diff = now - i->last_call;

						if (diff < i->interval)
						{
							++i;
							continue;
						}

						i->last_call = now;

						const auto res = i->handler();
						if (res == cond_end)
						{
							i = tasks.erase(i);
						}
						else
						{
							++i;
						}
					}
				});
			}

		private:
			utils::concurrency::container<task_list> new_callbacks_;
			utils::concurrency::container<task_list, std::recursive_mutex> callbacks_;

			void merge_callbacks()
			{
				callbacks_.access([&](task_list& tasks)
				{
					new_callbacks_.access([&](task_list& new_tasks)
					{
							tasks.insert(tasks.end(), std::move_iterator<task_list::iterator>(new_tasks.begin()),
								std::move_iterator<task_list::iterator>(new_tasks.end()));
						new_tasks = {};
					});
				});
			}
		};

		std::atomic_bool kill{false};
		// Avoid a joinable std::thread destructor running from the DLL's CRT
		// on-exit table when process teardown bypasses component cleanup.
		std::thread* thread = nullptr;
		task_pipeline pipelines[pipeline::count];
		void* console_rect_original = nullptr;
		bool renderer_call_installed = false;
		constexpr std::array<unsigned char, 5> renderer_call_bytes{0xE8, 0x60, 0x16, 0x00, 0x00};
		//utils::hook::detour g_run_frame_hook;
		utils::hook::detour main_frame_hook;
		utils::hook::detour g_shutdown_game_hook;

		std::vector<std::function<void()>> shutdown_callbacks;
		quit_boundary pending_quit;
		utils::hook::detour quit_command_hook;
		utils::hook::detour outer_frame_hook;
		void* outer_frame_original = nullptr;

		void stop_async_scheduler()
		{
			kill = true;
			const auto scheduler_thread = std::exchange(thread, nullptr);
			if (!scheduler_thread) return;
			if (scheduler_thread->joinable()) scheduler_thread->join();
			delete scheduler_thread;
		}

		int quit_command_stub()
		{
			pending_quit.request();
			return 0;
		}

		bool quit_at_frame_boundary()
		{
			if (pending_quit.begin_quit())
			{
				// Stop engine-using async callbacks before native shutdown deletes
				// the dvar hash table and all Sys critical sections.
				stop_async_scheduler();
				quit_command_hook.invoke<int>();
			}
			return pending_quit.stopped();
		}

		__declspec(naked) void outer_frame_stub()
		{
			__asm
			{
				pushfd
				pushad
				mov esi, esp
				sub esp, 528
				and esp, -16
				fxsave [esp]
				call quit_at_frame_boundary
				test al, al
				fxrstor [esp]
				mov esp, esi
				jnz finished
				popad
				popfd
				jmp dword ptr [outer_frame_original]
			finished:
				popad
				popfd
				ret
			}
		}

		bool game_window_closed()
		{
			static bool saw_game_window = false;
			static auto window_missing_since = std::chrono::steady_clock::time_point{};

			const auto hwnd = *game::main_window;
			if (hwnd && IsWindow(hwnd))
			{
				saw_game_window = true;
				window_missing_since = {};
				return false;
			}

			if (!saw_game_window)
			{
				return false;
			}

			if (window_missing_since == std::chrono::steady_clock::time_point{})
			{
				window_missing_since = std::chrono::steady_clock::now();
				return false;
			}

			return (std::chrono::steady_clock::now() - window_missing_since) > std::chrono::seconds(3);
		}

		void execute(const pipeline type)
		{
			if (pending_quit.stopped()) return;
			assert(type >= 0 && type < pipeline::count);
			if (type < 0 || type >= pipeline::count)
			{
				return;
			}

			pipelines[type].execute();
		}

		void main_frame_stub()
		{
			main_frame_hook.invoke<void>();

			const auto _0 = gsl::finally([]()
			{
				execute(pipeline::main);
			});
		}

		void render_frame_callbacks()
		{
			execute(pipeline::renderer);
		}

		// Con_SetConsoleRect has live SIMD state at this call site. Preserve the
		// complete native register/FPU state, then tail-call its original entry.
		__declspec(naked) void render_frame_stub()
		{
			__asm
			{
				pushfd
				pushad
				mov esi, esp
				sub esp, 528
				and esp, -16
				fxsave [esp]
				call render_frame_callbacks
				fxrstor [esp]
				mov esp, esi
				popad
				popfd
				jmp dword ptr [console_rect_original]
			}
		}

		
		void g_shutdown_game_stub(const int free_scripts)
		{
			g_shutdown_game_hook.invoke<void>(free_scripts);

			for (const auto& callback : shutdown_callbacks)
			{
				callback();
			}
		}
		
	}

	void on_shutdown(const std::function<void()>& callback)
	{
		shutdown_callbacks.push_back(callback);
	}

	void schedule(const std::function<bool()>& callback, const pipeline type,
	              const std::chrono::milliseconds delay)
	{
		assert(type >= 0 && type < pipeline::count);

		task task;
		task.handler = callback;
		task.interval = delay;
		task.last_call = std::chrono::high_resolution_clock::now();

		pipelines[type].add(std::move(task));
	}

	void loop(const std::function<void()>& callback, const pipeline type,
	          const std::chrono::milliseconds delay)
	{
		schedule([callback]()
		{
			callback();
			return cond_continue;
		}, type, delay);
	}

	void once(const std::function<void()>& callback, const pipeline type,
	          const std::chrono::milliseconds delay)
	{
		schedule([callback]()
		{
			callback();
			return cond_end;
		}, type, delay);
	}

	class component final : public component_interface
	{
	public:
		void post_start() override
		{
			kill = false;
			thread = new std::thread(utils::thread::create_named_thread("Async Scheduler", []()
			{
				while (!kill)
				{
					if (game_window_closed())
					{
						kill = true;
						return;
					}

					execute(pipeline::async);
					std::this_thread::sleep_for(10ms);
				}
			}));
		}

		void post_load() override
		{
			// QoS PC 1.1: 0x103F8660 is registered as quit;
			// its returning Sys_Quit (0x102453F0) deletes critical sections.
			// ntdll.dmp captured Dvar_FindVar from UI_SetActiveMenu AFTER that
			// teardown. The outer frame (0x103F9740), called by startMainMP,
			// is outside UI and Cbuf execution. Defer quit there and return to
			// the native loop, which checks exit byte 0x11A76581 immediately.
			// Unlike IW3's exit(0), QoS returns; never resume its current frame.
			// Keep this adapter until native quit/frame ownership is replaced.
			quit_command_hook.create(game::game_offset(0x103F8660), quit_command_stub);
			outer_frame_hook.create(game::game_offset(0x103F9740), outer_frame_stub);
			outer_frame_original = outer_frame_hook.get_original();
			// QoS PC 1.1 Con_DrawConsole synchronizes at 10311F77, builds the
			// frontend list, then closes/submits it at 10312239/1031225B.
			// 103C1050 runs BEFORE its own render-thread wait: appending there
			// races command consumers (stackk.dmp: version text became a header).
			// Like KisakCOD R_EndFrame, drawing must precede list finalization.
			// Keep this call-site adapter until frontend ownership is replaced.
			const auto renderer_call = game::game_offset(0x103121BB);
			if (std::memcmp(reinterpret_cast<const void*>(renderer_call),
				renderer_call_bytes.data(), renderer_call_bytes.size()) != 0)
				throw std::runtime_error("Unsupported frontend renderer callback site");
			console_rect_original = reinterpret_cast<void*>(game::game_offset(0x10313820));
			utils::hook::call(renderer_call, render_frame_stub);
			renderer_call_installed = true;
			main_frame_hook.create(game::game_offset(0x103F7470), scheduler::main_frame_stub); // may be wrong?

			g_shutdown_game_hook.create(game::game_offset(0x101AB0B0), g_shutdown_game_stub);
		}

		void pre_destroy() override
		{
			stop_async_scheduler();
			if (renderer_call_installed)
			{
				utils::hook::set(game::game_offset(0x103121BB), renderer_call_bytes);
				renderer_call_installed = false;
			}
		}
	};
}

REGISTER_COMPONENT(scheduler::component)
