#pragma once

namespace filesystem
{
	std::string read_file(const std::string& path);
	bool read_file(const std::string& path, std::string* data, std::string* real_path = nullptr);
	bool find_file(const std::string& path, std::string* real_path);
	bool exists(const std::string& path);

	void enable_engine_search_paths(bool enable);
	bool engine_search_paths_enabled();

	void register_path(const std::filesystem::path& path);
	void unregister_path(const std::filesystem::path& path);

	std::vector<std::string> get_search_paths();
	std::vector<std::string> get_search_paths_rev();
}
