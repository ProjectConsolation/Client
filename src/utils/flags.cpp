#include <std_include.hpp>
#include "flags.hpp"
#include "string.hpp"

#include <winternl.h>

namespace utils::flags
{
	bool query_process_basic_information(HANDLE process, PROCESS_BASIC_INFORMATION& information)
	{
		using query_t = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
		static const auto query = reinterpret_cast<query_t>(
			GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
		return query && query(process, ProcessBasicInformation, &information,
			sizeof(information), nullptr) >= 0;
	}

	std::wstring parent_launcher_command_line()
	{
		PROCESS_BASIC_INFORMATION current_info{};
		if (!query_process_basic_information(GetCurrentProcess(), current_info))
		{
			return {};
		}

		const auto parent_pid = static_cast<DWORD>(
			reinterpret_cast<ULONG_PTR>(current_info.Reserved3));
		const auto parent = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
			FALSE, parent_pid);
		if (!parent)
		{
			return {};
		}

		std::wstring result;
		do
		{
			std::array<wchar_t, 32768> image_path{};
			DWORD image_path_chars = static_cast<DWORD>(image_path.size());
			if (!QueryFullProcessImageNameW(parent, 0, image_path.data(), &image_path_chars)
				|| _wcsicmp(std::filesystem::path(image_path.data()).filename().c_str(),
					L"JB_Launcher_s.exe") != 0)
			{
				break;
			}

			PROCESS_BASIC_INFORMATION parent_info{};
			if (!query_process_basic_information(parent, parent_info))
			{
				break;
			}

			PEB peb{};
			SIZE_T bytes_read = 0;
			if (!ReadProcessMemory(parent, parent_info.PebBaseAddress, &peb,
				sizeof(peb), &bytes_read) || bytes_read != sizeof(peb) || !peb.ProcessParameters)
			{
				break;
			}

			RTL_USER_PROCESS_PARAMETERS parameters{};
			if (!ReadProcessMemory(parent, peb.ProcessParameters, &parameters,
				sizeof(parameters), &bytes_read) || bytes_read != sizeof(parameters))
			{
				break;
			}

			const auto& remote = parameters.CommandLine;
			constexpr USHORT max_command_line_bytes = 32766 * sizeof(wchar_t);
			if (!remote.Buffer || remote.Length == 0 || remote.Length > max_command_line_bytes
				|| (remote.Length % sizeof(wchar_t)) != 0)
			{
				break;
			}

			result.resize(remote.Length / sizeof(wchar_t));
			if (!ReadProcessMemory(parent, remote.Buffer, result.data(), remote.Length,
				&bytes_read) || bytes_read != remote.Length)
			{
				result.clear();
			}
		} while (false);

		CloseHandle(parent);
		return result;
	}

	std::vector<std::string> parse_arguments(const wchar_t* command_line)
	{
		std::vector<std::string> arguments;
		if (!command_line || !*command_line)
		{
			return arguments;
		}

		int num_args = 0;
		const auto argv = CommandLineToArgvW(command_line, &num_args);
		if (!argv)
		{
			return arguments;
		}

		for (auto i = 1; i < num_args; ++i)
		{
			const std::wstring wide_arg(argv[i]);
			std::string argument;
			const auto required = WideCharToMultiByte(CP_UTF8, 0, wide_arg.c_str(), -1,
				nullptr, 0, nullptr, nullptr);
			if (required > 1)
			{
				argument.resize(required);
				WideCharToMultiByte(CP_UTF8, 0, wide_arg.c_str(), -1,
					argument.data(), required, nullptr, nullptr);
				argument.pop_back();
			}
			arguments.emplace_back(std::move(argument));
		}

		LocalFree(argv);
		return arguments;
	}

	const std::vector<std::string>& get_launch_arguments()
	{
		static const auto arguments = []
		{
			const auto launcher_command_line = parent_launcher_command_line();
			return parse_arguments(launcher_command_line.empty()
				? GetCommandLineW()
				: launcher_command_line.c_str());
		}();
		return arguments;
	}

	void parse_flags(std::vector<std::string>& flags)
	{

		flags.clear();
		for (const auto& argument : get_launch_arguments())
		{
			if (!argument.empty() && argument[0] == '-')
			{
				flags.emplace_back(argument.begin() + 1, argument.end());
			}
		}
	}

	bool has_flag(const std::string& flag)
	{
		static auto parsed = false;
		static std::vector<std::string> enabled_flags;

		if (!parsed)
		{
			parse_flags(enabled_flags);
			parsed = true;
		}

		for (const auto& entry : enabled_flags)
		{
			if (string::to_lower(entry) == string::to_lower(flag))
			{
				return true;
			}
		}

		return false;
	}
}
