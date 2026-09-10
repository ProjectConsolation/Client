#include <std_include.hpp>

#include "scaleform.hpp"

#include <utils/memory.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

#include <climits>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace scaleform
{
	namespace
	{
		std::unordered_map<std::string, game::RawFile*> loaded_rawfiles;
		std::mutex loaded_rawfiles_mutex;

		std::string normalize_path(const char* name)
		{
			auto path = utils::string::to_lower(name ? name : "");
			std::replace(path.begin(), path.end(), '\\', '/');
			if (!path.ends_with(".gfx") || path.empty() || path.front() == '/' || path.find(':') != std::string::npos)
			{
				return {};
			}

			for (const auto& part : std::filesystem::path(path))
			{
				if (part == "." || part == "..")
				{
					return {};
				}
			}
			return path;
		}

		game::RawFile* make_rawfile(const std::string& name, const std::string& data)
		{
			auto* rawfile = utils::memory::allocate<game::RawFile>();
			auto* rawfile_name = static_cast<char*>(utils::memory::allocate(name.size() + 1));
			auto* buffer = static_cast<char*>(utils::memory::allocate(data.size()));
			std::memcpy(rawfile_name, name.data(), name.size());
			std::memcpy(buffer, data.data(), data.size());
			rawfile_name[name.size()] = '\0';
			rawfile->name = rawfile_name;
			rawfile->len = static_cast<unsigned int>(data.size());
			rawfile->buffer = buffer;
			return rawfile;
		}

		game::RawFile* load_override(const std::string& name)
		{
			std::lock_guard lock(loaded_rawfiles_mutex);
			if (const auto it = loaded_rawfiles.find(name); it != loaded_rawfiles.end())
			{
				return it->second;
			}

			static const auto root = std::filesystem::path(utils::nt::library(game::mp_dll).get_folder()) / "consolation";
			std::ifstream stream(root / name, std::ios::binary | std::ios::ate);
			const std::streamsize size = stream ? stream.tellg() : 0;
			if (size <= 0 || size > INT_MAX)
			{
				return nullptr;
			}
			std::string data(static_cast<std::size_t>(size), '\0');
			stream.seekg(0, std::ios::beg);
			if (!stream.read(data.data(), size))
			{
				return nullptr;
			}
			auto* rawfile = make_rawfile(name, data);
			loaded_rawfiles.emplace(name, rawfile);
			return rawfile;
		}

	}

	game::RawFile* try_override(const char* name)
	{
		const auto normalized = normalize_path(name);
		return normalized.empty() ? nullptr : load_override(normalized);
	}

	void clear_overrides()
	{
		std::lock_guard lock(loaded_rawfiles_mutex);
		loaded_rawfiles.clear();
	}

}
