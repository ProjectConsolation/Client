#pragma once

namespace fastfiles::xenon
{
	// Only the verified v470 UI loading-zone profile is supported, not map worlds.
	bool prepare(const std::filesystem::path& source, const std::string& zone_name);
	std::optional<HANDLE> open_prepared(const char* file_name, DWORD access, DWORD sharing,
		LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file);
	void clear();
}
