#pragma once
#include <string>
#include <string_view>
#include <cstring>

namespace gametypes::labels
{
	// QoS mode .txt files contain a display token followed by optional fields
	// (team, maxPlayers, ...). Match the native UI loader's first-token parsing.
	inline std::string display_token(std::string_view input, std::string_view fallback)
	{
		if (const auto zero = input.find('\0'); zero != input.npos) input = input.substr(0, zero);
		while (true)
		{
			const auto first = input.find_first_not_of(" \r\n\t");
			if (first == input.npos) return std::string(fallback);
			input.remove_prefix(first);
			if (!input.starts_with("//")) break;
			const auto end = input.find('\n');
			if (end == input.npos) return std::string(fallback);
			input.remove_prefix(end + 1);
		}
		if (input.front() == '"')
		{
			input.remove_prefix(1);
			const auto end = input.find('"');
			if (end == input.npos) return std::string(fallback);
			input = input.substr(0, end);
		}
		else input = input.substr(0, input.find_first_of(" \r\n\t"));
		return input.empty() ? std::string(fallback) : std::string(input);
	}

	inline bool hidden_mode(std::string_view id, bool debug)
	{
		return !debug && id.size() == 4
			&& (id[0] == 'm' || id[0] == 'M') && (id[1] == 'e' || id[1] == 'E')
			&& (id[2] == 'n' || id[2] == 'N') && (id[3] == 'u' || id[3] == 'U');
	}

	inline int filter_modes(char* rows, int count, int capacity, bool debug)
	{
		if (count < 0 || count > capacity) return count;
		int written = 0;
		for (int i = 0; i < count; ++i)
		{
			const char* row = rows + 44 * i;
			std::size_t length = 0;
			while (length < 12 && row[length]) ++length;
			if (hidden_mode(std::string_view(row, length), debug)) continue;
			if (written != i) std::memmove(rows + 44 * written, row, 44);
			++written;
		}
		std::memset(rows + 44 * written, 0, 44 * (count - written));
		return written;
	}
	// Scaleform/custom rows can retain quotes/@ from authored menu references.
	// Normalize a reference, then use the current language's native StringEd
	// lookup. Literal custom names and missing keys remain readable, unchanged.
	template <typename Lookup>
	std::string resolve(std::string_view input, Lookup lookup)
	{
		if (const auto zero = input.find('\0'); zero != input.npos) input = input.substr(0, zero);
		const auto first = input.find_first_not_of(" \r\n\t");
		if (first == input.npos) return {};
		input = input.substr(first, input.find_last_not_of(" \r\n\t") - first + 1);
		if (input.front() == '"')
		{
			// Also recover already-cached labels containing trailing mode fields.
			const auto end = input.find('"', 1);
			if (end != input.npos) input = input.substr(1, end - 1);
		}
		if (input.empty()) return {};
		if (input.front() == '\x15') return std::string(input.substr(1));
		if (input.front() == '@') input.remove_prefix(1);
		const std::string reference(input);
		if (const auto* translated = lookup(reference.c_str()); translated && *translated)
			return translated;
		return reference;
	}
}
