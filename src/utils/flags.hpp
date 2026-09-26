#pragma once

namespace utils::flags
{
	bool has_flag(const std::string& flag);
	const std::vector<std::string>& get_launch_arguments();
}
