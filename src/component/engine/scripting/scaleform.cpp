#include <std_include.hpp>

#include "scaleform.hpp"
#include "component/engine/console/console.hpp"

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
		struct override_cache
		{
			std::mutex mutex;
			std::unordered_map<std::string, game::RawFile*> rawfiles;
		};

		override_cache& get_override_cache()
		{
			// Component cleanup also runs from CRT exit, after ordinary globals
			// in this translation unit may have been destroyed.
			static auto* cache = new override_cache;
			return *cache;
		}

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
			auto& cache = get_override_cache();
			std::lock_guard lock(cache.mutex);
			if (const auto it = cache.rawfiles.find(name); it != cache.rawfiles.end())
			{
				return it->second;
			}

			static const auto root = std::filesystem::path(utils::nt::library(game::mp_dll).get_folder()) / "consolation";
			const auto disk_path = root / name;
			std::ifstream stream(disk_path, std::ios::binary | std::ios::ate);
			const auto size = stream ? static_cast<std::streamoff>(stream.tellg()) : 0;
			if (size <= 0 || size > INT_MAX)
			{
				if (name == "scaleform/mpmainmenu.gfx")
				{
					console::warn("[scaleform - override] unavailable or invalid size: %s\n", disk_path.string().c_str());
				}
				return nullptr;
			}
			std::string data(static_cast<std::size_t>(size), '\0');
			stream.seekg(0, std::ios::beg);
			if (!stream.read(data.data(), static_cast<std::streamsize>(size)))
			{
				console::warn("[scaleform - override] read failed: %s\n", disk_path.string().c_str());
				return nullptr;
			}
			auto* rawfile = make_rawfile(name, data);
			cache.rawfiles.emplace(name, rawfile);
			console::info("[scaleform - override] loaded %s (%u bytes)\n", disk_path.string().c_str(), rawfile->len);
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
		auto& cache = get_override_cache();
		std::lock_guard lock(cache.mutex);
		cache.rawfiles.clear();
	}

}
