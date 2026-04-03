#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include <utils/nt.hpp>

namespace export_
{
	namespace
	{
		using init_t = void(__cdecl*)();
		using shutdown_t = void(__cdecl*)();
		using is_initialized_t = int(__cdecl*)();

		struct state
		{
			HMODULE module = nullptr;
			init_t init = nullptr;
			shutdown_t shutdown = nullptr;
			is_initialized_t is_initialized = nullptr;
			bool init_succeeded = false;
			bool missing_logged = false;
			bool symbol_error_logged = false;
			bool load_error_logged = false;
			bool console_opened = false;
			bool waiting_logged = false;
			bool visibility_reduced = false;
			bool stop_requested = false;
			std::thread worker{};
		};

		state loader_state{};

		void log_line(const char* fmt, ...)
		{
			char buffer[1024]{};
			va_list ap{};
			va_start(ap, fmt);
			vsnprintf_s(buffer, _TRUNCATE, fmt, ap);
			va_end(ap);

			OutputDebugStringA(buffer);
			std::printf("%s", buffer);
		}

		bool is_enabled()
		{
			const auto* const command_line = GetCommandLineA();
			return command_line && std::strstr(command_line, "-xport") != nullptr;
		}

		void ensure_console_visible()
		{
			if (loader_state.console_opened)
			{
				return;
			}

			if (!GetConsoleWindow())
			{
				AllocConsole();
				AttachConsole(GetCurrentProcessId());

				FILE* stream = nullptr;
				freopen_s(&stream, "CONIN$", "r", stdin);
				freopen_s(&stream, "CONOUT$", "w", stdout);
				freopen_s(&stream, "CONOUT$", "w", stderr);
			}

			const auto console_window = GetConsoleWindow();
			if (console_window)
			{
				ShowWindow(console_window, SW_SHOW);
				SetForegroundWindow(console_window);
			}

			loader_state.console_opened = true;
		}

		HWND get_main_window()
		{
			if (!GetModuleHandleA("jb_mp_s.dll"))
			{
				return nullptr;
			}

			return *game::main_window;
		}

		bool is_game_ready_for_export()
		{
			if (!GetModuleHandleA("jb_mp_s.dll"))
			{
				return false;
			}

			const auto window = get_main_window();
			if (!window || !IsWindow(window))
			{
				return false;
			}

			if (!game::Dvar_FindVar("version"))
			{
				return false;
			}

			if (!game::cmd_functions || !*game::cmd_functions)
			{
				return false;
			}

			return true;
		}

		void reduce_game_visibility()
		{
			if (loader_state.visibility_reduced)
			{
				return;
			}

			const auto window = get_main_window();
			if (!window || !IsWindow(window))
			{
				return;
			}

			ShowWindow(window, SW_MINIMIZE);
			loader_state.visibility_reduced = true;
		}

		std::filesystem::path get_default_dll_path()
		{
			const auto host_folder = std::filesystem::path(utils::nt::get_host_module().get_folder());
			if (!host_folder.empty())
			{
				return host_folder / "qos-xport.dll";
			}

			return std::filesystem::current_path() / "qos-xport.dll";
		}

		bool resolve_exports()
		{
			if (!loader_state.module)
			{
				return false;
			}

			loader_state.init = reinterpret_cast<init_t>(GetProcAddress(loader_state.module, "qos_xport_init"));
			loader_state.shutdown = reinterpret_cast<shutdown_t>(GetProcAddress(loader_state.module, "qos_xport_shutdown"));
			loader_state.is_initialized = reinterpret_cast<is_initialized_t>(GetProcAddress(loader_state.module, "qos_xport_is_initialized"));

			if (!loader_state.init || !loader_state.shutdown || !loader_state.is_initialized)
			{
				if (!loader_state.symbol_error_logged)
				{
					log_line("export: qos-xport.dll is missing one or more expected exports\n");
					loader_state.symbol_error_logged = true;
				}

				FreeLibrary(loader_state.module);
				loader_state.module = nullptr;
				loader_state.init = nullptr;
				loader_state.shutdown = nullptr;
				loader_state.is_initialized = nullptr;
				return false;
			}

			return true;
		}

		bool ensure_module_loaded()
		{
			if (loader_state.module)
			{
				return true;
			}

			const auto dll_path = get_default_dll_path();
			if (!std::filesystem::exists(dll_path))
			{
				if (!loader_state.missing_logged)
				{
					log_line("export: qos-xport.dll not found at %s\n", dll_path.generic_string().c_str());
					loader_state.missing_logged = true;
				}

				return false;
			}

			loader_state.module = LoadLibraryA(dll_path.generic_string().c_str());
			if (!loader_state.module)
			{
				if (!loader_state.load_error_logged)
				{
					log_line("export: failed to load %s (error %lu)\n", dll_path.generic_string().c_str(), GetLastError());
					loader_state.load_error_logged = true;
				}

				return false;
			}

			log_line("export: loaded %s\n", dll_path.generic_string().c_str());
			return resolve_exports();
		}

		void worker_thread()
		{
			ensure_console_visible();

			while (!loader_state.stop_requested)
			{
				if (!loader_state.waiting_logged)
				{
					log_line("export: -xport enabled, waiting for game startup before loading qos-xport.dll\n");
					loader_state.waiting_logged = true;
				}

				if (is_game_ready_for_export())
				{
					if (ensure_module_loaded())
					{
						if (loader_state.is_initialized && !loader_state.is_initialized())
						{
							loader_state.init();
						}

						if (loader_state.is_initialized && loader_state.is_initialized())
						{
							loader_state.init_succeeded = true;
							log_line("export: initialized qos-xport.dll successfully\n");
							reduce_game_visibility();
						}
						else
						{
							log_line("export: qos_xport_init completed but exporter did not report initialized\n");
						}
					}

					return;
				}

				std::this_thread::sleep_for(250ms);
			}
		}

		void shutdown_loader()
		{
			loader_state.stop_requested = true;

			if (loader_state.worker.joinable())
			{
				loader_state.worker.join();
			}

			if (loader_state.module && loader_state.shutdown && loader_state.init_succeeded)
			{
				loader_state.shutdown();
				log_line("export: shut down qos-xport.dll\n");
			}

			if (loader_state.module)
			{
				FreeLibrary(loader_state.module);
			}

			loader_state = {};
		}
	}

	class component final : public component_interface
	{
	public:
		bool is_supported() override
		{
			return true;
		}

		void post_start() override
		{
			if (!is_enabled())
			{
				return;
			}

			loader_state.worker = std::thread(worker_thread);
		}

		void pre_destroy() override
		{
			shutdown_loader();
		}
	};
}

REGISTER_COMPONENT(export_::component)
