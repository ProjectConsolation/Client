#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>

#define OUTPUT_HANDLE GetStdHandle(STD_OUTPUT_HANDLE)

namespace console
{
	namespace
	{
		utils::hook::detour printf_hook;
		utils::hook::detour com_printf_hook;

		std::recursive_mutex print_mutex;

		struct
		{
			bool kill;
			std::thread thread;
			HANDLE kill_event;
			char buffer[512]{};
			int cursor;
			std::int32_t history_index = -1;
			std::deque<std::string> history{};
		} con{};

		void set_cursor_pos(int x)
		{
			CONSOLE_SCREEN_BUFFER_INFO info{};
			GetConsoleScreenBufferInfo(OUTPUT_HANDLE, &info);
			info.dwCursorPosition.X = static_cast<short>(x);
			SetConsoleCursorPosition(OUTPUT_HANDLE, info.dwCursorPosition);
		}

		void show_cursor(const bool show)
		{
			CONSOLE_CURSOR_INFO info{};
			GetConsoleCursorInfo(OUTPUT_HANDLE, &info);
			info.bVisible = show;
			SetConsoleCursorInfo(OUTPUT_HANDLE, &info);
		}

		template <typename... Args>
		int invoke_printf(const char* fmt, Args&&... args)
		{
			return printf_hook.invoke<int>(fmt, std::forward<Args>(args)...);
		}

		std::string format(va_list* ap, const char* message)
		{
			static thread_local char buffer[0x1000];

			const auto count = vsnprintf_s(buffer, _TRUNCATE, message, *ap);
			if (count < 0)
			{
				return {};
			}

			return { buffer, static_cast<size_t>(count) };
		}

		uint8_t get_attribute(const int type)
		{
			switch (type)
			{
			case con_type_info:
				return 7; // white
			case con_type_warning:
				return 6; // yellow
			case con_type_error:
				return 4; // red
			case con_type_debug:
				return 3; // cyan
			}

			return 7;
		}

		void update()
		{
			std::lock_guard _0(print_mutex);

			show_cursor(false);
			set_cursor_pos(0);
			invoke_printf("%s", con.buffer);
			set_cursor_pos(con.cursor);
			show_cursor(true);
		}

		void clear_output()
		{
			std::lock_guard _0(print_mutex);

			show_cursor(false);
			set_cursor_pos(0);

			for (auto i = 0; i < static_cast<int>(std::strlen(con.buffer)); i++)
			{
				invoke_printf(" ");
			}

			set_cursor_pos(con.cursor);
			show_cursor(true);
		}

		int dispatch_message(const int type, const std::string& message)
		{
			std::lock_guard _0(print_mutex);

			clear_output();
			set_cursor_pos(0);

			SetConsoleTextAttribute(OUTPUT_HANDLE, get_attribute(type));
			const auto res = invoke_printf("%s", message.data());
			SetConsoleTextAttribute(OUTPUT_HANDLE, get_attribute(con_type_info));

			if (message.size() <= 0 || message[message.size() - 1] != '\n')
			{
				invoke_printf("\n");
			}

			update();
			return res;
		}

		void clear()
		{
			std::lock_guard _0(print_mutex);

			clear_output();
			strncpy_s(con.buffer, "", sizeof(con.buffer));

			con.cursor = 0;
			set_cursor_pos(0);
		}

		size_t get_max_input_length()
		{
			CONSOLE_SCREEN_BUFFER_INFO info{};
			GetConsoleScreenBufferInfo(OUTPUT_HANDLE, &info);
			const auto columns = static_cast<size_t>(info.srWindow.Right - info.srWindow.Left - 1);
			return std::max(size_t(0), std::min(columns, sizeof(con.buffer)));
		}

		void handle_resize()
		{
			clear();
			update();
		}

		void handle_input(const INPUT_RECORD record)
		{
			if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
			{
				handle_resize();
				return;
			}

			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
			{
				return;
			}

			std::lock_guard _0(print_mutex);

			const auto key = record.Event.KeyEvent.wVirtualKeyCode;
			switch (key)
			{
			case VK_UP:
			{
				if (++con.history_index >= static_cast<int>(con.history.size()))
				{
					con.history_index = static_cast<int>(con.history.size()) - 1;
				}

				clear();

				if (con.history_index != -1)
				{
					strncpy_s(con.buffer, con.history.at(con.history_index).data(), sizeof(con.buffer));
					con.cursor = static_cast<int>(strlen(con.buffer));
				}

				update();
				break;
			}
			case VK_DOWN:
			{
				if (--con.history_index < -1)
				{
					con.history_index = -1;
				}

				clear();

				if (con.history_index != -1)
				{
					strncpy_s(con.buffer, con.history.at(con.history_index).data(), sizeof(con.buffer));
					con.cursor = static_cast<int>(strlen(con.buffer));
				}

				update();
				break;
			}
			case VK_LEFT:
			{
				if (con.cursor > 0)
				{
					con.cursor--;
					set_cursor_pos(con.cursor);
				}

				break;
			}
			case VK_RIGHT:
			{
				if (con.cursor < static_cast<int>(std::strlen(con.buffer)))
				{
					con.cursor++;
					set_cursor_pos(con.cursor);
				}

				break;
			}
			case VK_RETURN:
			{
				if (con.history_index != -1)
				{
					const auto itr = con.history.begin() + con.history_index;

					if (*itr == con.buffer)
					{
						con.history.erase(con.history.begin() + con.history_index);
					}
				}

				if (con.buffer[0])
				{
					con.history.push_front(con.buffer);
				}

				if (con.history.size() > 10)
				{
					con.history.erase(con.history.begin() + 10);
				}

				con.history_index = -1;

				game::Cbuf_AddText(0, utils::string::va("%s\n", con.buffer));

				con.cursor = 0;

				clear_output();
				SetConsoleTextAttribute(OUTPUT_HANDLE, get_attribute(con_type_info));
				invoke_printf("]%s\r\n", con.buffer);
				SetConsoleTextAttribute(OUTPUT_HANDLE, get_attribute(con_type_info));
				strncpy_s(con.buffer, "", sizeof(con.buffer));
				break;
			}
			case VK_BACK:
			{
				if (con.cursor <= 0)
				{
					break;
				}

				clear_output();

				std::memmove(con.buffer + con.cursor - 1, con.buffer + con.cursor,
					strlen(con.buffer) + 1 - con.cursor);
				con.cursor--;

				update();
				break;
			}
			case VK_ESCAPE:
			{
				con.cursor = 0;
				clear_output();
				strncpy_s(con.buffer, "", sizeof(con.buffer));
				break;
			}
			default:
			{
				const auto c = record.Event.KeyEvent.uChar.AsciiChar;
				if (!c)
				{
					break;
				}

				if (std::strlen(con.buffer) + 1 >= get_max_input_length())
				{
					break;
				}

				std::memmove(con.buffer + con.cursor + 1,
					con.buffer + con.cursor, std::strlen(con.buffer) + 1 - con.cursor);
				con.buffer[con.cursor] = c;
				con.cursor++;

				update();
				break;
			}
			}
		}

		int __cdecl printf_stub(const char* fmt, ...)
		{
			va_list ap;
			va_start(ap, fmt);
			const auto result = format(&ap, fmt);
			va_end(ap);

			game::CL_ConsolePrint(0, 6, result.c_str(), 0, 0, 0);

			//game::Com_Printf(0, "%s", result.data()); // this eventually leads to Com_PrintMessage, which leads to com_printf_stub below and will dispatch to our console

			return invoke_printf("%s", result.data()); // return the status of normal printf, which is what we are hooking
		}

		void com_printf_stub(int channel, const char* fmt, ...)
		{
			va_list ap;
			va_start(ap, fmt);
			const auto result = format(&ap, fmt);
			va_end(ap);

			com_printf_hook.invoke<void>(channel, "%s", result); // invoke Com_PrintMessage with the clean result

			// TODO: add dvar to stop this function from running

			// pretty sure channel 4 is used for debugging, but the normal user doesn't usually see it. remove this if you want every single print.
			//if (channel != 4)
			{
				std::string new_result(result);

				// parse color strings from the game's text to support our console
				int type = con_type_info;
				if (utils::string::starts_with(result, "^1"))
				{
					type = con_type_error;
				}
				else if (utils::string::starts_with(result, "^3"))
				{
					type = con_type_warning;
				}
				else if (utils::string::starts_with(result, "^5"))
				{
					type = con_type_debug;
				}

				if (type != con_type_info)
				{
					new_result.erase(0, 2); // example: '^1'
				}

				if (utils::string::string_contains(result, "Unable to")
					|| utils::string::string_contains(result, "Couldn't reset")
					|| utils::string::string_contains(result, "nat type is Strict"))
				{
					type = con_type_error;
				}

				if (utils::string::string_contains(result, "Unable to")
					|| utils::string::string_contains(result, "Missing asset")
					|| utils::string::string_contains(result, "Couldn't find")
					|| utils::string::string_contains(result, "Unknown command")
					|| utils::string::string_contains(result, "is read only")
					|| utils::string::string_contains(result, "is not a valid value")
					|| utils::string::string_contains(result, "is write protected")
					|| utils::string::string_contains(result, "already defined")
					|| utils::string::string_contains(result, "Adding channel:")
					|| utils::string::string_contains(result, "Hiding channel:")
					|| utils::string::string_contains(result, "will use the shock")
					|| utils::string::string_contains(result, "WARNING:")
					|| utils::string::string_contains(result, "nat type is Moderate"))
					{
						type = con_type_warning;
					}

				if (utils::string::string_contains(result, "GamerProfile_LogInProfile took")
					|| utils::string::string_contains(result, "connection status has changed")
					|| utils::string::string_contains(result, "signed in to live")
					|| utils::string::string_contains(result, "Reading profile")
					|| utils::string::string_contains(result, "Loaded zone")
					|| utils::string::string_contains(result, "Loading fastfile")
					|| utils::string::string_contains(result, "nat type is Open")
					|| utils::string::string_contains(result, "LOADING...")
					|| utils::string::string_contains(result, "xuid")
					|| utils::string::string_contains(result, "execing"))
					{
						type = con_type_debug;
					}

				static bool xsignin = false;


				if (utils::string::string_contains(result, "Velocity change:"))
				{
					auto g_debugVelocity = game::Dvar_FindVar("g_debugVelocity");
					if (g_debugVelocity->current.enabled)
					{
						dispatch_message(con_type_debug, result);
					}
				}


				if (utils::string::string_contains(result, "XUserReadProfileSettings"))
				{
					if (xsignin == false)
					{
						//reduce xsignin spam
						dispatch_message(con_type_debug, "Reading XUserProfileSettings");
						xsignin = true;

						if (utils::string::string_contains(result, "took 0ms"))
						{
							return;
						}
						else
						{
							dispatch_message(con_type_debug, result);
						}
					}
				}
				else
					dispatch_message(type, new_result);
			}
		}
	}

	void print(const int type, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		const auto result = format(&ap, fmt);
		va_end(ap);

		dispatch_message(type, result);
	}

	bool match_compare(const std::string& input, const std::string& text, const bool exact)
	{
		if (exact && text == input) return true;
		if (!exact && text.find(input) != std::string::npos) return true;
		return false;
	}

	void showDevConsole()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		game::Sys_ShowConsole();

		MSG message;
		while (GetMessageA(&message, 0, 0, 0))
		{
			TranslateMessage(&message);
			DispatchMessageA(&message);
		}
		//exit(0);
	}

	class component final : public component_interface
	{
	public:
		component()
		{
			AllocConsole();
			AttachConsole(GetCurrentProcessId());

			freopen_s((FILE**)stdin, "CONIN$", "r", stdin);
			freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);
			freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);

			ShowWindow(GetConsoleWindow(), SW_HIDE);
		}

		void post_load() override
		{
			// redirect console output to our console
			printf_hook.create(printf, printf_stub);
//#ifndef DEBUG
			com_printf_hook.create(game::game_offset(0x103F6400), com_printf_stub);
//#endif
			// setup external console
			ShowWindow(GetConsoleWindow(), SW_SHOW);
			SetConsoleTitle("Project: Consolation - Console");

			con.kill_event = CreateEvent(NULL, TRUE, FALSE, NULL);

			con.thread = utils::thread::create_named_thread("Console", []()
			{
				const auto handle = GetStdHandle(STD_INPUT_HANDLE);
				HANDLE handles[2] = {handle, con.kill_event};
				MSG msg{};

				INPUT_RECORD record{};
				DWORD num_events{};

				while (!con.kill)
				{
					const auto result = MsgWaitForMultipleObjects(2, handles, FALSE, INFINITE, QS_ALLINPUT);
					if (con.kill)
					{
						return;
					}

					switch (result)
					{
					case WAIT_OBJECT_0:
					{
						if (!ReadConsoleInput(handle, &record, 1, &num_events) || num_events == 0)
						{
							break;
						}

						handle_input(record);
						break;
					}
					case WAIT_OBJECT_0 + 1:
					{
						if (!PeekMessageA(&msg, GetConsoleWindow(), NULL, NULL, PM_REMOVE))
						{
							break;
						}

						if (msg.message == WM_QUIT)
						{
							game::Cbuf_AddText(0, "quit\n");
							break;
						}

						TranslateMessage(&msg);
						DispatchMessage(&msg);
						break;
					}
					}
				}
			});
		}

		void pre_destroy() override
		{
			con.kill = true;
			SetEvent(con.kill_event);

			if (con.thread.joinable())
			{
				con.thread.join();
			}
		}
	};
}

REGISTER_COMPONENT(console::component)
