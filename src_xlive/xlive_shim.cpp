#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	using XUID = unsigned long long;

	constexpr DWORD success = ERROR_SUCCESS;
	constexpr DWORD io_pending = ERROR_IO_PENDING;
	constexpr DWORD no_more_files = ERROR_NO_MORE_FILES;
	constexpr DWORD not_found = ERROR_NOT_FOUND;
	constexpr DWORD invalid_parameter = ERROR_INVALID_PARAMETER;
	constexpr DWORD insufficient_buffer = ERROR_INSUFFICIENT_BUFFER;
	constexpr DWORD qos_title_id = 0x41560829;
	constexpr DWORD xuser_data_type_int32 = 1;
	constexpr DWORD xuser_data_type_int64 = 2;
	constexpr DWORD xuser_data_type_double = 3;
	constexpr DWORD xuser_data_type_unicode = 4;
	constexpr DWORD xuser_data_type_float = 5;
	constexpr DWORD xuser_data_type_binary = 6;
	constexpr DWORD xuser_data_type_datetime = 7;
	constexpr DWORD xuser_data_type_null = 0xFF;
	constexpr DWORD xsource_no_value = 0;
	constexpr DWORD xsource_title = 2;
	constexpr DWORD xonline_e_storage_file_not_found = 0x8015C004;
	constexpr DWORD xonline_e_storage_file_is_too_big = 0x8015C003;
	constexpr DWORD signed_in_to_live = 2;
	constexpr DWORD xuser_name_size = 16;
	constexpr DWORD xnet_connect_status_connected = 2;
	constexpr DWORD xnet_get_xnaddr_ethernet = 0x00000002;
	constexpr DWORD xnet_get_xnaddr_static = 0x00000008;
	constexpr DWORD xnet_get_xnaddr_online = 0x00000080;
	constexpr BYTE xnet_qos_info_complete = 0x01;
	constexpr BYTE xnet_qos_info_target_contacted = 0x02;
	constexpr std::uintptr_t qos_base = 0x10000000;
	constexpr std::uintptr_t dvar_hash_table_addr = 0x1149FCE0;
	constexpr std::uintptr_t generate_hash_value_addr = 0x10275260;
	WORD system_link_port = 3074;

	struct dvar_value
	{
		union
		{
			bool enabled;
			int integer;
			float value;
			const char* string;
			float vector[4];
			char color[4];
		};
	};

	struct dvar_s
	{
		const char* name;
		const char* description;
		std::uint16_t flags;
		char pad[2];
		std::uint8_t type;
		bool modified;
		dvar_value current;
		dvar_value latched;
		dvar_value reset;
		std::uint8_t domain[8];
		dvar_s* next;
		dvar_s* hash_next;
	};
	static_assert(offsetof(dvar_s, current) == 16);
	static_assert(offsetof(dvar_s, hash_next) == 76);

	struct xoverlapped
	{
		ULONG_PTR internal_low;
		ULONG_PTR internal_high;
		ULONG_PTR internal_context;
		HANDLE event;
		void (WINAPI* completion_routine)(DWORD, DWORD, xoverlapped*);
		DWORD_PTR completion_context;
		DWORD extended_error;
	};
	static_assert(sizeof(xoverlapped) == 28);

	struct xnaddr
	{
		IN_ADDR local_address;
		IN_ADDR online_address;
		WORD online_port;
		BYTE ethernet_address[6];
		BYTE online_id[20];
	};
	static_assert(sizeof(xnaddr) == 36);

	struct xsession_info
	{
		BYTE session_id[8];
		xnaddr host_address;
		BYTE key_exchange_key[16];
	};
	static_assert(sizeof(xsession_info) == 60);

	#pragma pack(push, 1)
	struct xstorage_download_results
	{
		DWORD bytes_total;
		XUID xuid_owner;
		FILETIME created;
	};
	#pragma pack(pop)
	static_assert(sizeof(xstorage_download_results) == 20);

	#pragma pack(push, 1)
	struct xnet_qos_info
	{
		BYTE flags;
		BYTE reserved;
		WORD probes_sent;
		WORD probes_received;
		WORD data_size;
		BYTE* data;
		WORD minimum_rtt_ms;
		WORD median_rtt_ms;
		DWORD upstream_bits_per_second;
		DWORD downstream_bits_per_second;
	};

	struct xnet_qos
	{
		UINT result_count;
		UINT pending_count;
		xnet_qos_info results[1];
	};
	#pragma pack(pop)

	struct xuser_data
	{
		BYTE type;
		union
		{
			LONG int32_value;
			LONGLONG int64_value;
			double double_value;
			struct
			{
				DWORD bytes;
				wchar_t* value;
			} string;
			float float_value;
			struct
			{
				DWORD bytes;
				BYTE* value;
			} binary;
			FILETIME datetime_value;
		};
	};
	static_assert(sizeof(xuser_data) == 24);

	struct xuser_profile_setting
	{
		DWORD source;
		union
		{
			DWORD user_index;
			XUID xuid;
		} user;
		DWORD setting_id;
		xuser_data data;
	};
	static_assert(sizeof(xuser_profile_setting) == 48);

	struct xuser_read_profile_setting_result
	{
		DWORD settings_size;
		xuser_profile_setting* settings;
	};
	static_assert(sizeof(xuser_read_profile_setting_result) == 8);

	struct xuser_signin_info
	{
		XUID xuid;
		DWORD info_flags;
		DWORD signin_state;
		DWORD guest_number;
		DWORD sponsor_user_index;
		char user_name[xuser_name_size];
	};
	static_assert(sizeof(xuser_signin_info) == 40);

	HMODULE qos_module()
	{
		return GetModuleHandleA("jb_mp_s.dll");
	}

	template <typename T>
	T qos_address(const std::uintptr_t ida_address)
	{
		const auto module = qos_module();
		if (!module)
		{
			return nullptr;
		}

		return reinterpret_cast<T>(reinterpret_cast<std::uintptr_t>(module) + ida_address - qos_base);
	}

	dvar_s* find_dvar(const char* name)
	{
		using generate_hash_value_t = int(__cdecl*)(char*);

		auto* const hash_table = qos_address<dvar_s**>(dvar_hash_table_addr);
		const auto generate_hash_value = qos_address<generate_hash_value_t>(generate_hash_value_addr);
		if (!hash_table || !generate_hash_value)
		{
			return nullptr;
		}

		char mutable_name[64]{};
		strncpy_s(mutable_name, name, _TRUNCATE);

		for (auto* var = hash_table[generate_hash_value(mutable_name)]; var; var = var->hash_next)
		{
			if (var->name && _stricmp(var->name, name) == 0)
			{
				return var;
			}
		}

		return nullptr;
	}

	std::string command_line_name()
	{
		const auto command_line = GetCommandLineA();
		if (!command_line)
		{
			return {};
		}

		int argc = 0;
		const auto argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argvw)
		{
			return {};
		}

		std::string result;
		for (int i = 0; i < argc; ++i)
		{
			char arg[256]{};
			WideCharToMultiByte(CP_ACP, 0, argvw[i], -1, arg, sizeof(arg), nullptr, nullptr);

			if ((_stricmp(arg, "name") == 0 || _stricmp(arg, "+name") == 0 || _stricmp(arg, "-name") == 0)
				&& i + 1 < argc)
			{
				char value[256]{};
				WideCharToMultiByte(CP_ACP, 0, argvw[i + 1], -1, value, sizeof(value), nullptr, nullptr);
				result = value;
				break;
			}

			if ((_stricmp(arg, "-set") == 0 || _stricmp(arg, "-seta") == 0) && i + 2 < argc)
			{
				char key[256]{};
				WideCharToMultiByte(CP_ACP, 0, argvw[i + 1], -1, key, sizeof(key), nullptr, nullptr);
				if (_stricmp(key, "name") == 0)
				{
					char value[256]{};
					WideCharToMultiByte(CP_ACP, 0, argvw[i + 2], -1, value, sizeof(value), nullptr, nullptr);
					result = value;
					break;
				}
			}
		}

		LocalFree(argvw);
		return result;
	}

	std::string offline_name()
	{
		if (auto* const name = find_dvar("name"); name && name->type == 7 && name->current.string && *name->current.string)
		{
			return name->current.string;
		}

		auto from_command_line = command_line_name();
		if (!from_command_line.empty())
		{
			return from_command_line;
		}

		return "Player";
	}

	XUID offline_xuid()
	{
		const auto name = offline_name();
		std::uint64_t hash = 1469598103934665603ull;
		for (const auto ch : name)
		{
			hash ^= static_cast<unsigned char>(ch);
			hash *= 1099511628211ull;
		}

		return 0xE000000000000000ull | (hash & 0x0000FFFFFFFFFFFFull);
	}

	std::filesystem::path storage_root()
	{
		HMODULE shim_module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&storage_root), &shim_module);

		std::array<wchar_t, 32768> module_path{};
		const auto length = GetModuleFileNameW(shim_module, module_path.data(), static_cast<DWORD>(module_path.size()));
		if (!length || length >= module_path.size())
		{
			return std::filesystem::current_path() / L"storage";
		}

		return std::filesystem::path(module_path.data()).parent_path() / L"storage";
	}

	std::wstring hex_id(const std::uint64_t value, const int width)
	{
		std::wostringstream stream;
		stream << std::uppercase << std::hex << std::setfill(L'0') << std::setw(width) << value;
		return stream.str();
	}

	std::filesystem::path offline_profile_root()
	{
		// Keep all emulated LIVE state portable beside the game executable.
		return storage_root() / L"profiles" / hex_id(offline_xuid(), 16);
	}

	std::filesystem::path profile_setting_path(const DWORD title_id, const DWORD setting_id)
	{
		const auto effective_title_id = title_id ? title_id : qos_title_id;
		return offline_profile_root() / L"settings" / hex_id(effective_title_id, 8)
			/ (hex_id(setting_id, 8) + L".bin");
	}

	bool read_file(const std::filesystem::path& path, std::vector<BYTE>& bytes)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
		{
			return false;
		}

		const auto size = file.tellg();
		if (size < 0)
		{
			return false;
		}

		bytes.resize(static_cast<std::size_t>(size));
		file.seekg(0);
		return bytes.empty() || static_cast<bool>(file.read(
			reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)));
	}

	bool write_file(const std::filesystem::path& path, const BYTE* bytes, const std::size_t size)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error)
		{
			return false;
		}

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			return false;
		}

		if (size)
		{
			file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
		}
		return static_cast<bool>(file);
	}

	std::uint16_t read_u16(const std::vector<BYTE>& bytes, const std::size_t offset)
	{
		std::uint16_t value{};
		memcpy(&value, bytes.data() + offset, sizeof(value));
		return value;
	}

	std::uint32_t read_u32(const std::vector<BYTE>& bytes, const std::size_t offset)
	{
		std::uint32_t value{};
		memcpy(&value, bytes.data() + offset, sizeof(value));
		return value;
	}

	std::uint64_t read_u64(const std::vector<BYTE>& bytes, const std::size_t offset)
	{
		std::uint64_t value{};
		memcpy(&value, bytes.data() + offset, sizeof(value));
		return value;
	}

	void import_legacy_qos_profile()
	{
		static XUID attempted_xuid = 0;
		const auto current_xuid = offline_xuid();
		if (attempted_xuid == current_xuid)
		{
			return;
		}
		attempted_xuid = current_xuid;

		wchar_t local_app_data[32768]{};
		if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, static_cast<DWORD>(std::size(local_app_data))))
		{
			return;
		}

		const auto content_root = std::filesystem::path(local_app_data) / L"Microsoft" / L"Xlive" / L"Content";
		std::error_code error;
		if (!std::filesystem::is_directory(content_root, error))
		{
			return;
		}

		for (std::filesystem::recursive_directory_iterator it(content_root, error), end; it != end && !error; it.increment(error))
		{
			if (!it->is_regular_file(error) || _wcsicmp(it->path().filename().c_str(), L"41560829.gpd") != 0)
			{
				continue;
			}

			std::vector<BYTE> gpd;
			if (!read_file(it->path(), gpd) || gpd.size() < 24 || memcmp(gpd.data(), "FBDX", 4) != 0)
			{
				return;
			}

			// GFWL uses the little-endian FBDX form of XDBF. Entry offsets are
			// relative to the data area following the fixed entry/free tables.
			const auto entry_capacity = read_u32(gpd, 8);
			const auto entry_count = read_u32(gpd, 12);
			const auto free_capacity = read_u32(gpd, 16);
			const std::size_t table_offset = 24;
			if (entry_count > entry_capacity || entry_capacity > (gpd.size() - table_offset) / 18)
			{
				return;
			}
			const auto free_table_offset = table_offset + static_cast<std::size_t>(entry_capacity) * 18;
			if (free_capacity > (gpd.size() - free_table_offset) / 8)
			{
				return;
			}
			const auto data_offset = free_table_offset + static_cast<std::size_t>(free_capacity) * 8;

			for (DWORD index = 0; index < entry_count; ++index)
			{
				const auto entry = table_offset + static_cast<std::size_t>(index) * 18;
				if (entry + 18 > gpd.size() || read_u16(gpd, entry) != 3)
				{
					continue;
				}

				const auto setting_id = static_cast<DWORD>(read_u64(gpd, entry + 2));
				const auto relative_value_offset = read_u32(gpd, entry + 10);
				const auto value_size = read_u32(gpd, entry + 14);
				if ((setting_id & 0xF0000000) != 0x60000000 || value_size < 24
					|| relative_value_offset > gpd.size() - data_offset)
				{
					continue;
				}
				const auto value_offset = data_offset + relative_value_offset;
				if (value_size > gpd.size() - value_offset)
				{
					continue;
				}

				// Namespace 3 binary settings have a 24-byte XUSER_DATA wrapper.
				const auto payload_size = read_u32(gpd, value_offset + 16);
				if (payload_size > value_size - 24)
				{
					continue;
				}

				const auto destination = profile_setting_path(qos_title_id, setting_id);
				if (!std::filesystem::exists(destination, error))
				{
					write_file(destination, gpd.data() + value_offset + 24, payload_size);
				}
			}
			return;
		}
	}

	std::wstring sanitize_storage_name(const wchar_t* name)
	{
		std::wstring result = name ? name : L"";
		for (auto& character : result)
		{
			if (character < 32 || wcschr(L"<>:\"/\\|?*", character))
			{
				character = L'_';
			}
		}
		if (result.empty() || result == L"." || result == L"..")
		{
			result = L"unnamed";
		}
		return result;
	}

	std::filesystem::path title_storage_path(const wchar_t* server_path)
	{
		constexpr wchar_t prefix[] = L"offline://";
		const wchar_t* value = server_path ? server_path : L"";
		if (_wcsnicmp(value, prefix, std::size(prefix) - 1) == 0)
		{
			value += std::size(prefix) - 1;
		}

		const auto separator = wcschr(value, L'/');
		const std::wstring facility = separator ? std::wstring(value, separator) : L"3";
		const auto* item_name = separator ? separator + 1 : value;
		return offline_profile_root() / L"title_storage" / sanitize_storage_name(facility.c_str())
			/ sanitize_storage_name(item_name);
	}

	std::vector<BYTE> serialize_profile_value(const xuser_data& data)
	{
		const BYTE* source = nullptr;
		std::size_t size = 0;
		switch (data.type)
		{
		case xuser_data_type_int32: source = reinterpret_cast<const BYTE*>(&data.int32_value); size = sizeof(data.int32_value); break;
		case xuser_data_type_int64: source = reinterpret_cast<const BYTE*>(&data.int64_value); size = sizeof(data.int64_value); break;
		case xuser_data_type_double: source = reinterpret_cast<const BYTE*>(&data.double_value); size = sizeof(data.double_value); break;
		case xuser_data_type_unicode: source = reinterpret_cast<const BYTE*>(data.string.value); size = data.string.bytes; break;
		case xuser_data_type_float: source = reinterpret_cast<const BYTE*>(&data.float_value); size = sizeof(data.float_value); break;
		case xuser_data_type_binary: source = data.binary.value; size = data.binary.bytes; break;
		case xuser_data_type_datetime: source = reinterpret_cast<const BYTE*>(&data.datetime_value); size = sizeof(data.datetime_value); break;
		default: break;
		}

		if (!source || !size)
		{
			return {};
		}
		return {source, source + size};
	}

	void complete_overlapped(xoverlapped* overlapped, const DWORD error = success, const DWORD bytes = 0)
	{
		if (!overlapped)
		{
			return;
		}

		overlapped->internal_low = error;
		overlapped->internal_high = bytes;
		overlapped->extended_error = error;
		if (overlapped->event)
		{
			SetEvent(overlapped->event);
		}
		if (overlapped->completion_routine)
		{
			overlapped->completion_routine(error, bytes, overlapped);
		}
	}

	DWORD finish_operation(xoverlapped* overlapped, const DWORD error = success, const DWORD bytes = 0)
	{
		complete_overlapped(overlapped, error, bytes);
		return overlapped ? io_pending : error;
	}

	void make_local_xnaddr(xnaddr* address, const IN_ADDR* requested_address = nullptr)
	{
		if (!address)
		{
			return;
		}

		memset(address, 0, sizeof(*address));
		const auto loopback = htonl(INADDR_LOOPBACK);
		address->local_address.s_addr = requested_address && requested_address->s_addr
			? requested_address->s_addr
			: loopback;
		address->online_address = address->local_address;
		address->online_port = htons(system_link_port);

		const auto xuid = offline_xuid();
		address->ethernet_address[0] = 0x02; // Locally administered, unicast MAC.
		for (std::size_t index = 1; index < std::size(address->ethernet_address); ++index)
		{
			address->ethernet_address[index] = static_cast<BYTE>(xuid >> (index * 8));
		}
		memcpy(address->online_id, &xuid, sizeof(xuid));
	}

	void make_session_id(void* session_id)
	{
		if (!session_id)
		{
			return;
		}

		const auto xuid = offline_xuid();
		memcpy(session_id, &xuid, sizeof(xuid));
		static_cast<BYTE*>(session_id)[0] &= 0x1F; // System-link XNKID class.
	}

	int make_qos_results(const UINT result_count, const DWORD bits_per_second, WSAEVENT event, xnet_qos** qos)
	{
		if (!qos)
		{
			return WSAEINVAL;
		}

		const auto allocation_count = std::max<UINT>(result_count, 1);
		const auto allocation_size = offsetof(xnet_qos, results)
			+ static_cast<std::size_t>(allocation_count) * sizeof(xnet_qos_info);
		auto* const value = static_cast<xnet_qos*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocation_size));
		if (!value)
		{
			*qos = nullptr;
			return WSAENOBUFS;
		}

		value->result_count = result_count;
		for (UINT index = 0; index < result_count; ++index)
		{
			auto& result = value->results[index];
			result.flags = xnet_qos_info_complete | xnet_qos_info_target_contacted;
			result.probes_sent = 1;
			result.probes_received = 1;
			result.minimum_rtt_ms = 1;
			result.median_rtt_ms = 1;
			result.upstream_bits_per_second = bits_per_second;
			result.downstream_bits_per_second = bits_per_second;
		}

		*qos = value;
		if (event != WSA_INVALID_EVENT)
		{
			WSASetEvent(event);
		}
		return 0;
	}

	void make_session_info(xsession_info* info)
	{
		if (!info)
		{
			return;
		}

		memset(info, 0, sizeof(*info));
		const auto xuid = offline_xuid();
		make_session_id(info->session_id);
		make_local_xnaddr(&info->host_address);
		for (std::size_t index = 0; index < std::size(info->key_exchange_key); ++index)
		{
			info->key_exchange_key[index] = static_cast<BYTE>((xuid >> ((index % 8) * 8)) + index * 17);
		}
	}

	DWORD inert_success(xoverlapped* overlapped = nullptr)
	{
		return finish_operation(overlapped);
	}

	DWORD inert_not_found(xoverlapped* overlapped = nullptr)
	{
		return finish_operation(overlapped, not_found);
	}
}

extern "C"
{
	int WINAPI xlive_XWSAStartup(WORD version, WSADATA* data) { return WSAStartup(version, data); }
	SOCKET WINAPI xlive_XSocketCreate(int af, int type, int protocol) { return socket(af, type, protocol); }
	int WINAPI xlive_XSocketClose(SOCKET socket_handle) { return closesocket(socket_handle); }
	int WINAPI xlive_XSocketIOCTLSocket(SOCKET socket_handle, long cmd, u_long* argp) { return ioctlsocket(socket_handle, cmd, argp); }
	int WINAPI xlive_XSocketSetSockOpt(SOCKET socket_handle, int level, int optname, const char* optval, int optlen) { return setsockopt(socket_handle, level, optname, optval, optlen); }
	int WINAPI xlive_XSocketBind(SOCKET socket_handle, const sockaddr* name, int namelen) { return bind(socket_handle, name, namelen); }
	int WINAPI xlive_XSocketRecvFrom(SOCKET socket_handle, char* buffer, int len, int flags, sockaddr* from, int* fromlen) { return recvfrom(socket_handle, buffer, len, flags, from, fromlen); }
	int WINAPI xlive_XSocketSendTo(SOCKET socket_handle, const char* buffer, int len, int flags, const sockaddr* to, int tolen) { return sendto(socket_handle, buffer, len, flags, to, tolen); }
	unsigned long WINAPI xlive_XSocketInet_Addr(const char* cp) { return inet_addr(cp); }
	int WINAPI xlive_XWSAGetLastError() { return WSAGetLastError(); }
	unsigned short WINAPI xlive_XSocketHTONS(unsigned short hostshort) { return htons(hostshort); }

	int WINAPI xlive_XNetStartup(void*) { return 0; }
	int WINAPI xlive_XNetXnAddrToInAddr(const xnaddr* address, const void*, IN_ADDR* in_addr)
	{
		if (!address || !in_addr)
		{
			return WSAEINVAL;
		}
		in_addr->s_addr = address->local_address.s_addr ? address->local_address.s_addr : htonl(INADDR_LOOPBACK);
		return 0;
	}
	int WINAPI xlive_XNetInAddrToXnAddr(IN_ADDR in_addr, xnaddr* address, void* session_id)
	{
		make_local_xnaddr(address, &in_addr);
		make_session_id(session_id);
		return 0;
	}
	int WINAPI xlive_XNetConnect(IN_ADDR) { return 0; }
	DWORD WINAPI xlive_XNetGetConnectStatus(IN_ADDR) { return xnet_connect_status_connected; }
	int WINAPI xlive_XNetQosListen(const void*, const BYTE*, UINT, DWORD, DWORD) { return 0; }
	int WINAPI xlive_XNetQosLookup(UINT console_count, const xnaddr* const*, const void* const*, const void* const*,
		UINT address_count, const IN_ADDR*, const DWORD*, UINT, DWORD bits_per_second, DWORD, WSAEVENT event, xnet_qos** qos)
	{
		return make_qos_results(console_count + address_count, bits_per_second, event, qos);
	}
	int WINAPI xlive_XNetQosServiceLookup(DWORD, WSAEVENT event, xnet_qos** qos)
	{
		return make_qos_results(0, 0, event, qos);
	}
	int WINAPI xlive_XNetQosRelease(xnet_qos* qos)
	{
		return !qos || HeapFree(GetProcessHeap(), 0, qos) ? 0 : WSAEINVAL;
	}
	DWORD WINAPI xlive_XNetGetTitleXnAddr(xnaddr* address)
	{
		make_local_xnaddr(address);
		return xnet_get_xnaddr_ethernet | xnet_get_xnaddr_static | xnet_get_xnaddr_online;
	}
	DWORD WINAPI xlive_XNetGetEthernetLinkStatus() { return 1; }
	DWORD WINAPI xlive_XNetSetSystemLinkPort(WORD port) { system_link_port = port; return success; }

	BOOL WINAPI xlive_XNotifyGetNext(HANDLE, DWORD, DWORD*, ULONG_PTR*) { return FALSE; }
	void WINAPI xlive_XNotifyPositionUI(DWORD) {}
	DWORD WINAPI xlive_XGetOverlappedExtendedError(xoverlapped* overlapped) { return overlapped ? overlapped->extended_error : GetLastError(); }
	DWORD WINAPI xlive_XGetOverlappedResult(xoverlapped* overlapped, DWORD* result, BOOL wait)
	{
		if (!overlapped)
		{
			return invalid_parameter;
		}
		if (overlapped->internal_low == io_pending && !wait)
		{
			return ERROR_IO_INCOMPLETE;
		}
		if (wait && overlapped->event)
		{
			WaitForSingleObject(overlapped->event, INFINITE);
		}
		if (result)
		{
			*result = static_cast<DWORD>(overlapped->internal_high);
		}
		return overlapped->extended_error;
	}

	DWORD WINAPI xlive_XLiveInput(void*) { return success; }
	DWORD WINAPI xlive_XLiveRender() { return success; }
	DWORD WINAPI xlive_XLiveOnCreateDevice(void*, void*) { return success; }
	DWORD WINAPI xlive_XLiveOnDestroyDevice() { return success; }
	DWORD WINAPI xlive_XLiveOnResetDevice(void*) { return success; }
	DWORD WINAPI xlive_XHVCreateEngine(void*, HANDLE* worker_thread, void** engine)
	{
		if (worker_thread)
		{
			*worker_thread = nullptr;
		}
		if (engine)
		{
			*engine = nullptr;
		}
		return E_NOTIMPL;
	}
	BOOL WINAPI xlive_XLivePreTranslateMessage(const MSG*) { return FALSE; }
	DWORD WINAPI xlive_XLiveCreateProtectedDataContext(void*, HANDLE* handle)
	{
		if (!handle)
		{
			return E_INVALIDARG;
		}
		*handle = reinterpret_cast<HANDLE>(1);
		return success;
	}
	DWORD WINAPI xlive_XLiveQueryProtectedDataInformation(HANDLE, DWORD* information)
	{
		if (!information || information[0] < sizeof(DWORD) * 2)
		{
			return E_INVALIDARG;
		}
		information[1] = 1; // XLIVE_PROTECTED_DATA_FLAG_OFFLINE_ONLY
		return success;
	}
	DWORD WINAPI xlive_XLiveCloseProtectedDataContext(HANDLE) { return success; }

	DWORD WINAPI xlive_XShowGuideUI(DWORD) { return success; }
	DWORD WINAPI xlive_XShowGamerCardUI(DWORD, XUID) { return success; }
	DWORD WINAPI xlive_XCancelOverlapped(xoverlapped* overlapped) { return finish_operation(overlapped, ERROR_OPERATION_ABORTED); }
	DWORD WINAPI xlive_XEnumerate(HANDLE, void*, DWORD, DWORD* returned, xoverlapped* overlapped) { if (returned) *returned = 0; return finish_operation(overlapped, no_more_files); }
	DWORD WINAPI xlive_XShowSigninUI(DWORD, DWORD) { return success; }

	DWORD WINAPI xlive_XUserGetXUID(DWORD user_index, XUID* xuid) { if (user_index != 0 || !xuid) return invalid_parameter; *xuid = offline_xuid(); return success; }
	DWORD WINAPI xlive_XUserGetSigninState(DWORD user_index) { return user_index == 0 ? signed_in_to_live : 0; }
	DWORD WINAPI xlive_XUserGetName(DWORD user_index, char* user_name, DWORD user_name_chars)
	{
		if (user_index != 0 || !user_name || user_name_chars == 0)
		{
			return invalid_parameter;
		}

		const auto name = offline_name();
		strncpy_s(user_name, user_name_chars, name.c_str(), _TRUNCATE);
		return success;
	}
	DWORD WINAPI xlive_XUserAreUsersFriends(DWORD, const XUID*, DWORD, void*, xoverlapped* overlapped) { return inert_not_found(overlapped); }
	DWORD WINAPI xlive_XUserCheckPrivilege(DWORD, DWORD, BOOL* allowed) { if (allowed) *allowed = TRUE; return success; }
	DWORD WINAPI xlive_XUserGetSigninInfo(DWORD user_index, DWORD, xuser_signin_info* info)
	{
		if (user_index != 0 || !info)
		{
			return invalid_parameter;
		}

		memset(info, 0, sizeof(*info));
		info->xuid = offline_xuid();
		info->info_flags = 1;
		info->signin_state = signed_in_to_live;
		strncpy_s(info->user_name, sizeof(info->user_name), offline_name().c_str(), _TRUNCATE);
		return success;
	}
	HANDLE WINAPI xlive_XNotifyCreateListener(ULONGLONG) { return CreateEventA(nullptr, FALSE, FALSE, nullptr); }
	DWORD WINAPI xlive_XShowFriendsUI(DWORD) { return success; }
	DWORD WINAPI xlive_XUserSetProperty(DWORD, DWORD, DWORD, const void*) { return success; }
	DWORD WINAPI xlive_XUserSetContext(DWORD, DWORD, DWORD) { return success; }
	DWORD WINAPI xlive_XUserWriteAchievements(DWORD, void*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XUserReadStats(DWORD, DWORD, const XUID*, DWORD, const void*, DWORD* result_size, void*, xoverlapped* overlapped)
	{
		if (result_size)
		{
			*result_size = 0;
		}
		return inert_not_found(overlapped);
	}
	DWORD WINAPI xlive_XUserCreateStatsEnumeratorByRank(DWORD, DWORD, DWORD, DWORD, const void*, DWORD* buffer_size, HANDLE* handle)
	{
		if (buffer_size)
		{
			*buffer_size = 0;
		}
		if (handle)
		{
			*handle = INVALID_HANDLE_VALUE;
		}
		return no_more_files;
	}
	DWORD WINAPI xlive_XUserCreateStatsEnumeratorByXuid(DWORD, XUID, DWORD, DWORD, const void*, DWORD* buffer_size, HANDLE* handle)
	{
		if (buffer_size)
		{
			*buffer_size = 0;
		}
		if (handle)
		{
			*handle = INVALID_HANDLE_VALUE;
		}
		return no_more_files;
	}
	DWORD WINAPI xlive_XUserResetStatsView(DWORD, DWORD, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XUserSetContextEx(DWORD, DWORD, DWORD, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XLivePBufferGetByteArray(void*, DWORD, BYTE* values, DWORD size) { if (values) memset(values, 0, size); return success; }
	DWORD WINAPI xlive_XLivePBufferSetByteArray(void*, DWORD, BYTE*, DWORD) { return success; }

	DWORD WINAPI xlive_XSessionCreate(DWORD, DWORD, DWORD, DWORD, ULONGLONG* nonce, xsession_info* info, xoverlapped* overlapped, HANDLE* handle)
	{
		if (!nonce || !info || !handle)
		{
			complete_overlapped(overlapped, invalid_parameter);
			return invalid_parameter;
		}

		*nonce = offline_xuid();
		make_session_info(info);
		*handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		if (!*handle)
		{
			const auto error = GetLastError();
			complete_overlapped(overlapped, error);
			return error;
		}
		return inert_success(overlapped);
	}
	DWORD WINAPI xlive_XStringVerify(DWORD, const char*, DWORD, const void*, DWORD, void*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XStorageUploadFromMemory(DWORD user_index, const wchar_t* server_path, DWORD buffer_size, const BYTE* buffer, xoverlapped* overlapped)
	{
		if (user_index != 0 || !server_path || (buffer_size && !buffer))
		{
			complete_overlapped(overlapped, invalid_parameter);
			return invalid_parameter;
		}

		const auto result = write_file(title_storage_path(server_path), buffer, buffer_size) ? success : ERROR_WRITE_FAULT;
		return finish_operation(overlapped, result, result == success ? buffer_size : 0);
	}
	DWORD WINAPI xlive_XFriendsCreateEnumerator(DWORD, DWORD, DWORD, DWORD* buffer_size, HANDLE* handle)
	{
		if (buffer_size)
		{
			*buffer_size = 0;
		}
		if (handle)
		{
			*handle = INVALID_HANDLE_VALUE;
		}
		return no_more_files;
	}
	DWORD WINAPI xlive_XUserMuteListQuery(DWORD, XUID, BOOL* muted) { if (muted) *muted = FALSE; return success; }
	DWORD WINAPI xlive_XInviteGetAcceptedInfo(DWORD, void*) { return not_found; }
	DWORD WINAPI xlive_XSessionWriteStats(HANDLE, XUID, DWORD, const void*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionStart(HANDLE, DWORD, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionSearchEx(DWORD, DWORD, DWORD, DWORD, WORD, WORD, const void*, const void*, DWORD* result_size, void*, xoverlapped* overlapped)
	{
		if (result_size)
		{
			*result_size = 0;
		}
		return inert_not_found(overlapped);
	}
	DWORD WINAPI xlive_XSessionModify(HANDLE, DWORD, DWORD, DWORD, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionMigrateHost(HANDLE, DWORD, xsession_info* info, xoverlapped* overlapped) { make_session_info(info); return inert_success(overlapped); }
	DWORD WINAPI xlive_XOnlineGetNatType() { return 1; }
	DWORD WINAPI xlive_XSessionJoinRemote(HANDLE, DWORD, const XUID*, const BOOL*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionDelete(HANDLE handle, xoverlapped* overlapped) { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); return inert_success(overlapped); }
	DWORD WINAPI xlive_XUserReadProfileSettings(DWORD title_id, DWORD user_index, DWORD setting_count, const DWORD* setting_ids,
		DWORD* result_size, xuser_read_profile_setting_result* result, xoverlapped* overlapped)
	{
		if (user_index != 0 || !result_size || (setting_count && !setting_ids))
		{
			complete_overlapped(overlapped, invalid_parameter);
			return invalid_parameter;
		}

		import_legacy_qos_profile();
		std::vector<std::vector<BYTE>> values(setting_count);
		std::vector<bool> value_present(setting_count);
		std::size_t required = sizeof(*result) + static_cast<std::size_t>(setting_count) * sizeof(xuser_profile_setting);
		for (DWORD index = 0; index < setting_count; ++index)
		{
			value_present[index] = read_file(profile_setting_path(title_id, setting_ids[index]), values[index]);
			const auto type = (setting_ids[index] >> 28) & 0xF;
			if (type == xuser_data_type_unicode || type == xuser_data_type_binary)
			{
				required += values[index].size();
			}
		}

		if (required > MAXDWORD)
		{
			complete_overlapped(overlapped, insufficient_buffer);
			return insufficient_buffer;
		}

		const auto required_size = static_cast<DWORD>(required);
		if (!result || *result_size < required_size)
		{
			*result_size = required_size;
			complete_overlapped(overlapped, insufficient_buffer);
			return insufficient_buffer;
		}

		memset(result, 0, required_size);
		result->settings_size = setting_count;
		result->settings = reinterpret_cast<xuser_profile_setting*>(reinterpret_cast<BYTE*>(result) + sizeof(*result));
		auto* variable_data = reinterpret_cast<BYTE*>(result->settings + setting_count);
		for (DWORD index = 0; index < setting_count; ++index)
		{
			auto& setting = result->settings[index];
			setting.user.user_index = user_index;
			setting.setting_id = setting_ids[index];
			if (!value_present[index])
			{
				setting.source = xsource_no_value;
				setting.data.type = static_cast<BYTE>(xuser_data_type_null);
				continue;
			}

			setting.source = xsource_title;
			setting.data.type = static_cast<BYTE>((setting.setting_id >> 28) & 0xF);
			const auto& value = values[index];
			switch (setting.data.type)
			{
			case xuser_data_type_int32:
				memcpy(&setting.data.int32_value, value.data(), std::min(value.size(), sizeof(setting.data.int32_value)));
				break;
			case xuser_data_type_int64:
				memcpy(&setting.data.int64_value, value.data(), std::min(value.size(), sizeof(setting.data.int64_value)));
				break;
			case xuser_data_type_double:
				memcpy(&setting.data.double_value, value.data(), std::min(value.size(), sizeof(setting.data.double_value)));
				break;
			case xuser_data_type_float:
				memcpy(&setting.data.float_value, value.data(), std::min(value.size(), sizeof(setting.data.float_value)));
				break;
			case xuser_data_type_datetime:
				memcpy(&setting.data.datetime_value, value.data(), std::min(value.size(), sizeof(setting.data.datetime_value)));
				break;
			case xuser_data_type_unicode:
				setting.data.string.bytes = static_cast<DWORD>(value.size());
				setting.data.string.value = reinterpret_cast<wchar_t*>(variable_data);
				memcpy(variable_data, value.data(), value.size());
				variable_data += value.size();
				break;
			case xuser_data_type_binary:
				setting.data.binary.bytes = static_cast<DWORD>(value.size());
				setting.data.binary.value = variable_data;
				memcpy(variable_data, value.data(), value.size());
				variable_data += value.size();
				break;
			default:
				setting.source = xsource_no_value;
				setting.data.type = static_cast<BYTE>(xuser_data_type_null);
				break;
			}
		}

		*result_size = required_size;
		return finish_operation(overlapped, success, required_size);
	}
	DWORD WINAPI xlive_XSessionEnd(HANDLE, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionArbitrationRegister(HANDLE, DWORD, ULONGLONG, DWORD*, void*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionLeaveRemote(HANDLE, DWORD, const XUID*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XUserWriteProfileSettings(DWORD user_index, DWORD setting_count, const xuser_profile_setting* settings, xoverlapped* overlapped)
	{
		if (user_index != 0 || (setting_count && !settings))
		{
			complete_overlapped(overlapped, invalid_parameter);
			return invalid_parameter;
		}

		for (DWORD index = 0; index < setting_count; ++index)
		{
			if (settings[index].data.type == xuser_data_type_null)
			{
				std::error_code error;
				std::filesystem::remove(profile_setting_path(qos_title_id, settings[index].setting_id), error);
				if (error)
				{
					complete_overlapped(overlapped, ERROR_WRITE_FAULT);
					return ERROR_WRITE_FAULT;
				}
				continue;
			}

			const auto value = serialize_profile_value(settings[index].data);
			if (!write_file(profile_setting_path(qos_title_id, settings[index].setting_id), value.data(), value.size()))
			{
				complete_overlapped(overlapped, ERROR_WRITE_FAULT);
				return ERROR_WRITE_FAULT;
			}
		}

		return finish_operation(overlapped);
	}
	DWORD WINAPI xlive_XSessionModifySkill(HANDLE, DWORD, const XUID*, xoverlapped* overlapped) { return inert_success(overlapped); }
	DWORD WINAPI xlive_XSessionCalculateSkill(DWORD count, double* mu, double* sigma, double* aggregate_mu, double* aggregate_sigma)
	{
		double mu_total = 0.0;
		double sigma_squared_total = 0.0;
		for (DWORD index = 0; index < count; ++index)
		{
			mu_total += mu ? mu[index] : 0.0;
			const auto value = sigma ? sigma[index] : 0.0;
			sigma_squared_total += value * value;
		}
		if (aggregate_mu)
		{
			*aggregate_mu = count ? mu_total / count : 0.0;
		}
		if (aggregate_sigma)
		{
			*aggregate_sigma = std::sqrt(sigma_squared_total);
		}
		return success;
	}
	DWORD WINAPI xlive_XStorageBuildServerPath(DWORD user_index, DWORD facility, const void*, DWORD, const wchar_t* item_name, wchar_t* server_path, DWORD* server_path_chars)
	{
		if (user_index != 0 || !item_name || !server_path_chars)
		{
			return invalid_parameter;
		}

		const auto path = std::wstring(L"offline://") + std::to_wstring(facility) + L"/" + sanitize_storage_name(item_name);
		const auto needed = static_cast<DWORD>(path.size() + 1);
		if (server_path_chars)
		{
			if (!server_path || *server_path_chars < needed)
			{
				*server_path_chars = needed;
				return ERROR_INSUFFICIENT_BUFFER;
			}
			*server_path_chars = needed;
		}
		if (server_path)
		{
			wcscpy_s(server_path, needed, path.c_str());
		}
		return success;
	}
	DWORD WINAPI xlive_XStorageDownloadToMemory(DWORD user_index, const wchar_t* server_path, DWORD buffer_size, BYTE* buffer, DWORD result_size, xstorage_download_results* results, xoverlapped* overlapped)
	{
		if (user_index != 0 || !server_path || (buffer_size && !buffer) || !results || result_size < sizeof(*results))
		{
			complete_overlapped(overlapped, invalid_parameter);
			return invalid_parameter;
		}

		std::vector<BYTE> value;
		if (!read_file(title_storage_path(server_path), value))
		{
			return finish_operation(overlapped, xonline_e_storage_file_not_found);
		}
		if (value.size() > buffer_size)
		{
			return finish_operation(overlapped, xonline_e_storage_file_is_too_big);
		}

		if (!value.empty())
		{
			memcpy(buffer, value.data(), value.size());
		}
		memset(results, 0, sizeof(*results));
		results->bytes_total = static_cast<DWORD>(value.size());
		results->xuid_owner = offline_xuid();
		GetSystemTimeAsFileTime(&results->created);
		return finish_operation(overlapped, success, static_cast<DWORD>(value.size()));
	}
}
