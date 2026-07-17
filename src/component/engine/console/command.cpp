#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "console.hpp"
#include "component/utils/scheduler.hpp"
#include "component/engine/scripting/gametypes.hpp"

#include <utils/memory.hpp>
#include <utils/string.hpp>
#include <utils/io.hpp>
#include "component/engine/zones/fastfiles.hpp"
#include <game/dvars.hpp>

namespace command
{
	namespace
	{
		static utils::memory::allocator cmd_allocator;
		constexpr int CLIENT_STRIDE = 688916;
		constexpr int CLIENT_REAL_PLAYER_OFFSET = 0x20;
		constexpr int CLIENT_USERINFO_OFFSET = 1604;
		constexpr int CLIENT_USERINFO_SIZE = 1024;
		constexpr int ACTIVE_PROFILE_INDEX = 0;
		constexpr auto BOT_NAMES_FILE = "consolation/bots.txt";
		constexpr auto LIVE_CLAN_TARGET_PRIMARY = 0x111CF170;
		constexpr auto LIVE_CLAN_TARGET_SECONDARY = 0x111F1C00;
		constexpr auto CLAN_DIRTY_FLAGS = 0x1149E6BC;
		constexpr auto GAMETYPES_RAWFILE = "maps/mp/gametypes/_gametypes.txt";

		std::unordered_map<std::string, std::function<void(params&)>> handlers;
		int next_bot_number = 1;
		std::vector<std::string> bot_names;

		bool parse_integer(const char* value, std::uintptr_t* result)
		{
			if (!value || value[0] == '\0' || !result)
			{
				return false;
			}

			auto base = 10;
			auto text = value;
			if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
			{
				base = 16;
			}
			else
			{
				for (auto* ch = text; *ch; ++ch)
				{
					if ((*ch >= 'a' && *ch <= 'f') || (*ch >= 'A' && *ch <= 'F'))
					{
						base = 16;
						break;
					}
				}
			}

			char* end = nullptr;
			const auto parsed = std::strtoull(text, &end, base);
			if (!end || *end != '\0')
			{
				return false;
			}

			*result = static_cast<std::uintptr_t>(parsed);
			return true;
		}

		std::uintptr_t resolve_address(std::uintptr_t parsed)
		{
			if (parsed >= 0x10000000 && parsed < 0x20000000)
			{
				return game::game_offset(parsed);
			}

			return parsed;
		}

		void append_format(std::string& output, const char* format, ...)
		{
			char buffer[256]{};
			va_list ap;
			va_start(ap, format);
			vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, ap);
			va_end(ap);
			output.append(buffer);
		}

		std::string get_exe_directory()
		{
			char path[MAX_PATH]{};
			const auto length = GetModuleFileNameA(nullptr, path, sizeof(path));
			if (length == 0 || length >= sizeof(path))
			{
				return {};
			}

			std::string result = path;
			const auto separator = result.find_last_of("/\\");
			if (separator == std::string::npos)
			{
				return {};
			}

			result.resize(separator);
			return result;
		}

		std::string resolve_game_path(const std::string& path)
		{
			if (path.size() > 2 && path[1] == ':')
			{
				return path;
			}

			const auto base = get_exe_directory();
			if (base.empty())
			{
				return path;
			}

			return base + "\\" + path;
		}

		bool create_directory_tree(const std::string& path, DWORD* error)
		{
			if (path.empty())
			{
				return true;
			}

			auto create_one = [error](const std::string& directory)
			{
				if (directory.empty() || (directory.size() == 2 && directory[1] == ':'))
				{
					return true;
				}

				if (CreateDirectoryA(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
				{
					return true;
				}

				if (error)
				{
					*error = GetLastError();
				}

				return false;
			};

			for (std::size_t i = 0; i < path.size(); ++i)
			{
				if (path[i] != '/' && path[i] != '\\')
				{
					continue;
				}

				if (!create_one(path.substr(0, i)))
				{
					return false;
				}
			}

			return create_one(path);
		}

		bool write_file_noexcept(const std::string& path, const std::string& data, DWORD* error)
		{
			const auto resolved_path = resolve_game_path(path);
			const auto separator = resolved_path.find_last_of("/\\");
			if (separator != std::string::npos && !create_directory_tree(resolved_path.substr(0, separator), error))
			{
				return false;
			}

			const auto file = CreateFileA(resolved_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				if (error)
				{
					*error = GetLastError();
				}

				return false;
			}

			DWORD written = 0;
			const auto size = static_cast<DWORD>(std::min<std::size_t>(data.size(), MAXDWORD));
			const auto ok = WriteFile(file, data.data(), size, &written, nullptr) && written == size;
			if (!ok && error)
			{
				*error = GetLastError();
			}

			CloseHandle(file);
			return ok;
		}

		bool is_readable_memory(const void* address, const std::size_t length)
		{
			if (!address || length == 0)
			{
				return false;
			}

			MEMORY_BASIC_INFORMATION info{};
			if (!VirtualQuery(address, &info, sizeof(info)))
			{
				return false;
			}

			const auto protection = info.Protect & 0xFF;
			const auto readable = protection == PAGE_READONLY
				|| protection == PAGE_READWRITE
				|| protection == PAGE_WRITECOPY
				|| protection == PAGE_EXECUTE_READ
				|| protection == PAGE_EXECUTE_READWRITE
				|| protection == PAGE_EXECUTE_WRITECOPY;

			const auto start = reinterpret_cast<std::uintptr_t>(address);
			const auto region_start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
			const auto region_end = region_start + info.RegionSize;
			return info.State == MEM_COMMIT && readable && start + length <= region_end;
		}

		bool safe_read_byte(const std::uintptr_t address, unsigned char* value)
		{
			if (!value)
			{
				return false;
			}

			SIZE_T bytes_read = 0;
			return ReadProcessMemory(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(address),
				value,
				sizeof(*value),
				&bytes_read) && bytes_read == sizeof(*value);
		}

		void dump_memory(const char* address_text, const char* length_text, const char* output_name)
		{
			if (!address_text || address_text[0] == '\0')
			{
				console::info("dumpMemory <address> [length] [output]: dump readable memory to consolation/memory_dump\n");
				console::info("dumpMemory accepts IDA addresses like 0x113D1684 or runtime pointers\n");
				return;
			}

			std::uintptr_t address = 0;
			std::size_t length = 0x100;
			std::uintptr_t parsed = 0;
			if (!parse_integer(address_text, &parsed))
			{
				console::error("dumpMemory: invalid address\n");
				return;
			}

			address = resolve_address(parsed);
			if (length_text && length_text[0] != '\0')
			{
				std::uintptr_t parsed_length = 0;
				if (!parse_integer(length_text, &parsed_length))
				{
					console::error("dumpMemory: invalid length\n");
					return;
				}

				length = static_cast<std::size_t>(parsed_length);
			}

			length = std::clamp<std::size_t>(length, 1, 0x4000);
			if (!is_readable_memory(reinterpret_cast<const void*>(address), 1))
			{
				console::error("dumpMemory: address %p is not readable\n", reinterpret_cast<void*>(address));
				return;
			}

			std::string output{};
			output.reserve((length / 16 + 1) * 80);
			for (std::size_t offset = 0; offset < length; offset += 16)
			{
				append_format(output, "%08X  ", static_cast<unsigned int>(address + offset));
				char ascii[17]{};
				for (std::size_t column = 0; column < 16; ++column)
				{
					if (offset + column < length)
					{
						unsigned char byte = 0;
						if (safe_read_byte(address + offset + column, &byte))
						{
							append_format(output, "%02X ", byte);
							ascii[column] = byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.';
						}
						else
						{
							output.append("?? ");
							ascii[column] = '?';
						}
					}
					else
					{
						output.append("   ");
					}
				}

				output.append(" ");
				for (std::size_t column = 0; column < 16 && offset + column < length; ++column)
				{
					output.push_back(ascii[column]);
				}

				output.append("\r\n");
			}

			std::string output_path = "consolation/memory_dump/";
			if (output_name && output_name[0] != '\0')
			{
				output_path.append(output_name);
			}
			else
			{
				char filename[32]{};
				sprintf_s(filename, "%08X.txt", static_cast<unsigned int>(address));
				output_path.append(filename);
			}

			DWORD error = ERROR_SUCCESS;
			if (write_file_noexcept(output_path, output, &error))
			{
				console::info("dumpMemory: wrote %s\n", output_path.c_str());
			}
			else
			{
				console::error("dumpMemory: failed to write %s (GetLastError=%lu)\n", output_path.c_str(), error);
			}
		}

		std::string normalize_rawfile_path(std::string path)
		{
			for (auto& ch : path)
			{
				if (ch == '\\')
				{
					ch = '/';
				}
			}

			return path;
		}

		std::string get_file_name(std::string path)
		{
			path = normalize_rawfile_path(std::move(path));
			const auto separator = path.find_last_of('/');
			if (separator == std::string::npos)
			{
				return path;
			}

			return path.substr(separator + 1);
		}

		std::string make_dump_safe_path(std::string path)
		{
			path = normalize_rawfile_path(std::move(path));
			while (!path.empty() && path.front() == '/')
			{
				path.erase(path.begin());
			}

			return path;
		}

		bool is_scaleform_asset_candidate(const char* asset_name)
		{
			if (!asset_name || asset_name[0] == '\0')
			{
				return false;
			}

			auto name = utils::string::to_lower(normalize_rawfile_path(asset_name));
			return name.find(".gfx") != std::string::npos
				|| name.find(".swf") != std::string::npos
				|| name.find("scaleform") != std::string::npos
				|| name.find("mpsysmodeselect") != std::string::npos
				|| name.find("mpxbplaylistselect") != std::string::npos
				|| name.find("cmsharedplatform") != std::string::npos
				|| name.find("gfxfontlib") != std::string::npos
				|| name.find("pcsharedlibrary") != std::string::npos
				|| name.find("cmsharedlibrary") != std::string::npos;
		}

		std::string get_rawfile_buffer(const game::RawFile* rawfile)
		{
			if (!rawfile || !rawfile->buffer)
			{
				return {};
			}

			if (rawfile->len > 0)
			{
				const auto size = rawfile->buffer[rawfile->len - 1] == '\0'
					? rawfile->len - 1
					: rawfile->len;
				return { rawfile->buffer, rawfile->buffer + size };
			}

			return rawfile->buffer;
		}

		std::string get_rawfile_binary_buffer(const game::RawFile* rawfile)
		{
			if (!rawfile || !rawfile->buffer || rawfile->len == 0)
			{
				return {};
			}

			return { rawfile->buffer, rawfile->buffer + rawfile->len };
		}

		void dump_rawfile(const char* rawfile_name, const char* output_name = nullptr)
		{
			if (!rawfile_name || rawfile_name[0] == '\0')
			{
				console::info("dumpRawFile <rawfile> [output]: dump a rawfile asset to consolation/rawfile_dump\n");
				return;
			}

			const auto normalized_name = normalize_rawfile_path(rawfile_name);
			const auto header = game::DB_FindXAssetHeader(game::ASSET_TYPE_RAWFILE, normalized_name.c_str());
			const auto data = get_rawfile_buffer(header.rawfile);
			if (data.empty())
			{
				console::warn("dumpRawFile: rawfile '%s' was empty or unavailable\n", normalized_name.c_str());
				return;
			}

			std::string output_path;
			if (output_name && output_name[0] != '\0')
			{
				output_path = "consolation/";
				output_path.append(output_name);
			}
			else
			{
				output_path = "consolation/rawfile_dump/";
				output_path.append(normalized_name);
			}

			if (utils::io::write_file(output_path, data))
			{
				console::info("dumpRawFile: dumped %s to %s\n", normalized_name.c_str(), output_path.c_str());
			}
			else
			{
				console::error("dumpRawFile: failed to write %s\n", output_path.c_str());
			}
		}

		bool dump_rawfile_binary(const game::RawFile* rawfile, const char* output_prefix)
		{
			if (!rawfile || !rawfile->name)
			{
				return false;
			}

			const auto data = get_rawfile_binary_buffer(rawfile);
			if (data.empty())
			{
				console::warn("dumpSwf: rawfile '%s' was empty or unavailable\n", rawfile->name);
				return false;
			}

			std::string output_path = output_prefix;
			output_path.append(make_dump_safe_path(rawfile->name));

			DWORD error = ERROR_SUCCESS;
			if (write_file_noexcept(output_path, data, &error))
			{
				console::info("dumpSwf: dumped %s to %s\n", rawfile->name, output_path.c_str());
				return true;
			}

			console::error("dumpSwf: failed to write %s (GetLastError=%lu)\n", output_path.c_str(), error);
			return false;
		}

		int dump_swf_by_name(const char* swf_name)
		{
			if (!swf_name || swf_name[0] == '\0')
			{
				console::info("dumpSwf <name.swf|name.gfx>: dump a loaded Scaleform rawfile to consolation/scaleform_dump\n");
				return 0;
			}

			const auto wanted = utils::string::to_lower(get_file_name(swf_name));
			auto fallback_wanted = wanted;
			if (fallback_wanted.ends_with(".swf"))
			{
				fallback_wanted.resize(fallback_wanted.size() - 4);
				fallback_wanted.append(".gfx");
			}

			auto dumped = 0;

			fastfiles::enum_assets(game::ASSET_TYPE_RAWFILE, [&](const game::XAssetHeader header)
			{
				const auto* const rawfile = header.rawfile;
				if (!rawfile || !rawfile->name)
				{
					return;
				}

				const auto file_name = utils::string::to_lower(get_file_name(rawfile->name));
				if (file_name != wanted && file_name != fallback_wanted)
				{
					return;
				}

				if (dump_rawfile_binary(rawfile, "consolation/scaleform_dump/"))
				{
					++dumped;
				}
			}, true);

			if (dumped == 0)
			{
				console::warn("dumpSwf: '%s' was not found in loaded Scaleform rawfile assets\n", swf_name);
			}

			return dumped;
		}

		void dump_scaleform_shared_files()
		{
			static constexpr const char* scaleform_names[]
			{
				"cmsharedplatform.gfx",
				"cmsharedlibrary.gfx",
				"pcsharedlibrary.gfx",
				"mpsharedlibrary.gfx",
				"cmfont1_glyphs.gfx",
				"cmfont2_glyphs.gfx",
				"cmfont3_glyphs.gfx",
				"cmfont4_glyphs.gfx",
				"cmfont5_glyphs.gfx",
			};

			auto total = 0;
			for (const auto* const scaleform_name : scaleform_names)
			{
				total += dump_swf_by_name(scaleform_name);
			}

			console::info("dumpScaleformSharedFiles: dumped %i file(s)\n", total);
		}

		void find_scaleform_assets(const char* filter)
		{
			const auto normalized_filter = filter && filter[0] != '\0'
				? utils::string::to_lower(normalize_rawfile_path(filter))
				: std::string{};

			auto total = 0;
			for (auto type_index = 0; type_index < game::ASSET_TYPE_COUNT; ++type_index)
			{
				const auto type = static_cast<game::XAssetType>(type_index);
				fastfiles::enum_assets(type, [type, type_index, &normalized_filter, &total](const game::XAssetHeader header)
				{
					game::XAsset asset{ type, header };
					const auto* const asset_name = game::DB_GetXAssetName(&asset);
					if (!asset_name || asset_name[0] == '\0')
					{
						return;
					}

					const auto normalized_name = utils::string::to_lower(normalize_rawfile_path(asset_name));
					if (!normalized_filter.empty())
					{
						if (normalized_name.find(normalized_filter) == std::string::npos)
						{
							return;
						}
					}
					else if (!is_scaleform_asset_candidate(asset_name))
					{
						return;
					}

					console::info("findScaleformAssets: type=%d:%s name=%s\n",
						type_index,
						game::g_assetNames[type_index],
						asset_name);
					++total;
				}, true);
			}

			console::info("findScaleformAssets: found %i asset(s)\n", total);
		}

		std::uintptr_t get_clients_base()
		{
			return *reinterpret_cast<std::uintptr_t*>(game::game_offset(0x11CA5D8C));
		}

		int get_max_clients()
		{
			if (const auto* dvar = game::Dvar_FindVar("sv_maxclients"); dvar)
			{
				return std::max(dvar->current.integer, 1);
			}

			return 18;
		}

		std::uintptr_t get_client(int index)
		{
			return get_clients_base() + static_cast<std::uintptr_t>(index) * CLIENT_STRIDE;
		}

		bool is_bot_client(int index)
		{
			const auto client = get_client(index);
			return *reinterpret_cast<int*>(client) >= game::CS_CONNECTED
				&& *reinterpret_cast<int*>(client + CLIENT_REAL_PLAYER_OFFSET) == 0;
		}

		std::string get_info_value(const std::string& info, const std::string& key)
		{
			size_t pos = 0;
			while (pos < info.size())
			{
				if (info[pos] == '\\')
				{
					++pos;
				}

				const auto key_end = info.find('\\', pos);
				if (key_end == std::string::npos)
				{
					break;
				}

				const auto value_end = info.find('\\', key_end + 1);
				const auto current_key = info.substr(pos, key_end - pos);
				const auto current_value = info.substr(
					key_end + 1,
					(value_end == std::string::npos ? info.size() : value_end) - (key_end + 1)
				);

				if (current_key == key)
				{
					return current_value;
				}

				if (value_end == std::string::npos)
				{
					break;
				}

				pos = value_end;
			}

			return {};
		}

		void set_info_value(std::string& info, const std::string& key, const std::string& value)
		{
			std::vector<std::pair<std::string, std::string>> pairs;
			bool replaced = false;
			size_t pos = 0;

			while (pos < info.size())
			{
				if (info[pos] == '\\')
				{
					++pos;
				}

				const auto key_end = info.find('\\', pos);
				if (key_end == std::string::npos)
				{
					break;
				}

				const auto value_end = info.find('\\', key_end + 1);
				auto current_key = info.substr(pos, key_end - pos);
				auto current_value = info.substr(
					key_end + 1,
					(value_end == std::string::npos ? info.size() : value_end) - (key_end + 1)
				);

				if (current_key == key)
				{
					current_value = value;
					replaced = true;
				}

				pairs.emplace_back(std::move(current_key), std::move(current_value));

				if (value_end == std::string::npos)
				{
					break;
				}

				pos = value_end;
			}

			if (!replaced)
			{
				pairs.emplace_back(key, value);
			}

			std::string rebuilt;
			for (const auto& [current_key, current_value] : pairs)
			{
				rebuilt.push_back('\\');
				rebuilt.append(current_key);
				rebuilt.push_back('\\');
				rebuilt.append(current_value);
			}

			info = std::move(rebuilt);
		}

		std::string trim_bot_name(std::string name)
		{
			while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ' || name.back() == '\t'))
			{
				name.pop_back();
			}

			size_t start = 0;
			while (start < name.size() && (name[start] == ' ' || name[start] == '\t'))
			{
				++start;
			}

			return name.substr(start);
		}

		void load_bot_names()
		{
			bot_names.clear();

			std::string data;
			if (!utils::io::read_file(BOT_NAMES_FILE, &data))
			{
				return;
			}

			size_t start = 0;
			while (start <= data.size())
			{
				const auto end = data.find('\n', start);
				auto line = data.substr(start, end == std::string::npos ? data.size() - start : end - start);
				line = trim_bot_name(std::move(line));

				if (!line.empty())
				{
					bot_names.emplace_back(std::move(line));
				}

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}
		}

		std::string get_next_bot_name()
		{
			if (!bot_names.empty())
			{
				const auto index = static_cast<size_t>((next_bot_number - 1) % bot_names.size());
				++next_bot_number;
				return bot_names[index];
			}

			return std::format("consolation_bot{}", next_bot_number++ - 1);
		}

		std::string normalize_clan_name(const std::string& raw_value)
		{
			std::string result{};
			result.reserve(4);

			for (const auto ch : raw_value)
			{
				if (result.size() >= 4)
				{
					break;
				}

				auto normalized = static_cast<unsigned char>(ch);
				if (normalized == '^')
				{
					normalized = ' ';
				}

				if (normalized < 32)
				{
					continue;
				}

				result.push_back(static_cast<char>(normalized));
			}

			return result;
		}

		const char* get_dvar_string(game::dvar_s* dvar)
		{
			return dvar && dvar->current.string ? dvar->current.string : "";
		}

		void update_clan_name_state(const std::string& clan_name)
		{
			game::Dvar_SetString("clanName", clan_name.c_str());
			game::GamerProfile_UpdateProfileFromDvars(ACTIVE_PROFILE_INDEX, 1);
			game::Live_UpdateClan(game::game_offset(LIVE_CLAN_TARGET_PRIMARY), const_cast<char*>(clan_name.c_str()));
			game::Live_UpdateClan(game::game_offset(LIVE_CLAN_TARGET_SECONDARY), const_cast<char*>(clan_name.c_str()));
			*reinterpret_cast<std::uint32_t*>(game::game_offset(CLAN_DIRTY_FLAGS)) |= 2u;
		}

		int rename_new_bot_client()
		{
			for (int i = 0; i < get_max_clients(); ++i)
			{
				if (!is_bot_client(i))
				{
					continue;
				}

				auto* const userinfo = reinterpret_cast<char*>(get_client(i) + CLIENT_USERINFO_OFFSET);
				const std::string info = userinfo;
				if (get_info_value(info, "name").rfind("bot", 0) != 0)
				{
					continue;
				}

				auto updated = info;
				set_info_value(updated, "name", get_next_bot_name());
				set_info_value(updated, "clanAbbrev", "CSL");

				std::memset(userinfo, 0, CLIENT_USERINFO_SIZE);
				std::memcpy(userinfo, updated.data(), std::min(updated.size(), static_cast<size_t>(CLIENT_USERINFO_SIZE - 1)));
				game::SV_ClientUserinfoChanged(i);
				return i;
			}

			return -1;
		}

		void main_handler()
		{
			params params = {};

			const auto command = utils::string::to_lower(params[0]);
			if (handlers.find(command) != handlers.end())
			{
				handlers[command](params);
			}
		}
	}

	params::params()
		: nesting_(*game::command_id)
	{
	}

	int params::size() const
	{
		return game::cmd_argc[this->nesting_];
	}

	const char* params::get(const int index) const
	{
		if (index >= this->size())
		{
			return "";
		}

		return game::cmd_argv[this->nesting_][index];
	}

	std::string params::join(const int index) const
	{
		std::string result = {};

		for (auto i = index; i < this->size(); i++)
		{
			if (i > index) result.append(" ");
			result.append(this->get(i));
		}
		return result;
	}

	std::vector<std::string> params::get_all() const
	{
		std::vector<std::string> params_;
		for (auto i = 0; i < this->size(); i++)
		{
			params_.push_back(this->get(i));
		}
		return params_;
	}

	void add_raw(const char* name, void (*callback)())
	{
		game::Cmd_AddCommandInternal(name, callback, cmd_allocator.allocate<game::cmd_function_s>());
	}

	void add(const char* name, const std::function<void(const params&)>& callback)
	{
		const auto command = utils::string::to_lower(name);

		if (handlers.find(command) == handlers.end())
			add_raw(name, main_handler);

		handlers[command] = callback;
	}

	void add(const char* name, const std::function<void()>& callback)
	{
		add(name, [callback](const params&)
			{
				callback();
			});
	}

	void execute(std::string command)
	{
		command += "\n";
		game::Cbuf_AddText(0, command.data());
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::once([&]()
				{
					load_bot_names();

					/*add("kick", [](const params& argument)
						{
							if (argument.size() < 2)
							{
								console::info("usage: kick <name>, <reason>(optional)\n");
								return;
							}
							std::string reason;
							if (reason.empty())
							{
								reason = "EXE_PLAYERKICKED";
							}

							game::SV_GameSendServerCommand(i, "r " + reason + "");
						});*/

					add("addbot", [](const params& args)
						{
							if (!game::SV_AddTestClient)
							{
								console::error("addbot: server not initialised\n");
								return;
							}

							load_bot_names();

							const int count = (args.size() >= 2) ? std::atoi(args[1]) : 1;
							if (count <= 0)
							{
								console::info("usage: addbot [count]\n");
								return;
							}

							int spawned = 0;
							for (int i = 0; i < count; ++i)
							{
								if (!game::SV_AddTestClient())
								{
									console::warn("addbot: server full after %i bot(s)\n", spawned);
									break;
								}

								const auto renamed_slot = rename_new_bot_client();
								if (renamed_slot < 0)
								{
									console::warn("addbot: spawned bot but failed to update its userinfo\n");
								}

								++spawned;
							}

							if (spawned > 0)
								console::info("addbot: spawned %i bot(s)\n", spawned);
						});

					add("clanName", [](const params& args)
						{
							auto* const clan_name = game::Dvar_FindVar("clanName");
							if (!clan_name)
							{
								console::error("clanName: dvar is unavailable\n");
								return;
							}

							if (args.size() < 2)
							{
								console::info("clanName: current tag is \"%s\"\n", get_dvar_string(clan_name));
								console::info("usage: clanName <tag>\n");
								return;
							}

							const auto normalized = normalize_clan_name(args.join(1));
							update_clan_name_state(normalized);
							console::info("clanName: set tag to \"%s\"\n", normalized.c_str());
						});

					add("dvarDump", [](const params& argument)
						{
							std::string filename;
							if (argument.size() == 2)
							{
								filename = "consolation/";
								filename.append(argument.get(1));
								if (!filename.ends_with(".txt"))
								{
									filename.append(".txt");
								}
							}

							console::info("================================ DVAR DUMP ========================================\n");
							for (auto i = 0; i < *game::dvarCount; i++)
							{
								auto* dvar = game::sortedDvars[i];

								if (dvar)
								{
									// TODO: fix this, there's a empty dvar (or multiple) caused string format crash which crashes game
									if (!filename.empty())
									{
										try
										{
											const auto line = std::format("{} \"{}\"\r\n", dvar->name, dvars::Dvar_ValueToString(dvar, dvar->current));
											utils::io::write_file(filename, line, i != 0);
										}
										catch (...)
										{

										}
									}

									try
									{
										console::info("%s \"%s\"\n", dvar->name, dvars::Dvar_ValueToString(dvar, dvar->current));
									}
									catch (...)
									{

									}
								}
							}

							console::info("\n%i dvars\n", *game::dvarCount);
							console::info("================================ END DVAR DUMP ====================================\n");
						});

					add("commandDump", [](const params& argument)
						{
							console::info("================================ COMMAND DUMP =====================================\n");
							game::cmd_function_s* cmd = (*game::cmd_functions);
							std::string filename;
							if (argument.size() == 2)
							{
								filename = "consolation/";
								filename.append(argument.get(1));
								if (!filename.ends_with(".txt"))
								{
									filename.append(".txt");
								}
							}
							int i = 0;
							while (cmd)
							{
								if (cmd->name)
								{
									if (!filename.empty())
									{
										const auto line = std::format("{}\r\n", cmd->name);
										utils::io::write_file(filename, line, i != 0);
									}
									console::info("%s\n", cmd->name);
									i++;
								}
								cmd = cmd->next;
							}
							console::info("\n%i commands\n", i);
							console::info("================================ END COMMAND DUMP =================================\n");
						});

					add("dumpRawFile", [](const params& argument)
						{
							if (argument.size() < 2)
							{
								dump_rawfile(nullptr);
								return;
							}

							dump_rawfile(argument.get(1), argument.get(2));
						});

					add("dumpSwf", [](const params& argument)
						{
							if (argument.size() < 2)
							{
								dump_swf_by_name(nullptr);
								return;
							}

							dump_swf_by_name(argument.get(1));
						});

					add("dumpScaleformSharedFiles", [](const params&)
						{
							dump_scaleform_shared_files();
						});

					add("findScaleformAssets", [](const params& argument)
						{
							find_scaleform_assets(argument.get(1));
						});

					add("dumpGametypes", [](const params&)
						{
							dump_rawfile(GAMETYPES_RAWFILE);
						});

					add("dumpGametypesDebug", [](const params&)
						{
							try
							{
								gametypes::dump_debug_state();
							}
							catch (...)
							{
								console::error("dumpGametypesDebug: unhandled exception\n");
							}
						});

					add("refreshGametypes", [](const params&)
						{
							try
							{
								gametypes::force_refresh_ui_gametype_list();
							}
							catch (...)
							{
								console::error("refreshGametypes: unhandled exception\n");
							}
						});

					add("dumpMemory", [](const params& argument)
						{
							try
							{
								if (argument.size() < 2)
								{
									dump_memory(nullptr, nullptr, nullptr);
									return;
								}

								dump_memory(argument.get(1), argument.get(2), argument.get(3));
							}
							catch (...)
							{
								console::error("dumpMemory: unhandled exception\n");
							}
						});

					add("listassetpool", [](const params& params)
						{
							if (params.size() < 2)
							{
								console::info("listassetpool <poolnumber> [filter]: list all the assets in the specified pool\n");

								for (auto i = 0; i < game::XAssetType::ASSET_TYPE_COUNT; i++)
								{
									console::info("ASSET; %d %s\n", i, game::g_assetNames[i]);
								}
							}
							else
							{
								const auto type = static_cast<game::XAssetType>(atoi(params.get(1)));
								if (type < 0 || type >= game::XAssetType::ASSET_TYPE_COUNT)
								{
									console::info("Invalid pool passed must be between [0, %d]\n", game::XAssetType::ASSET_TYPE_COUNT - 1);
									return;
								}

								console::info("Listing assets in pool %s\n", game::g_assetNames[type]);

								auto total_assets = 0;
								const std::string filter = params.get(2);

								fastfiles::enum_assets(type, [type, filter](const game::XAssetHeader header)
									{
										auto asset = game::XAsset{ type, header };
										const auto* const asset_name = game::DB_GetXAssetName(&asset);

										if (!filter.empty() && !console::match_compare(filter, asset_name, false))
										{
											return;
										}
										console::info("%s\n", asset_name);
									}, true);
							}
						});

				}, scheduler::main);
		}

		void pre_destroy() override
		{
			cmd_allocator.clear();
		}
	};
}

REGISTER_COMPONENT(command::component)
