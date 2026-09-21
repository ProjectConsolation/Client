#pragma once

namespace fastfiles::xenon
{
	// Converts supported Xenon v470 profiles into temporary PC v470 zones and
	// exposes explicitly selected PC v470 conversion probes through the same cache.
	// Map manifests are validated, but incompatible map records are rejected until
	// their field-wise PC serializers are complete.
	bool prepare(const std::filesystem::path& source, const std::string& zone_name, bool allow_pc_probe = false);
	std::optional<HANDLE> open_prepared(const char* file_name, DWORD access, DWORD sharing,
		LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file);
	void clear();
}
