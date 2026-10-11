#pragma once
#include <string>
#include <string_view>

namespace gametypes::labels
{
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
		if (input.size() >= 2 && input.front() == '"' && input.back() == '"')
			input = input.substr(1, input.size() - 2);
		if (input.empty()) return {};
		if (input.front() == '\x15') return std::string(input.substr(1));
		if (input.front() == '@') input.remove_prefix(1);
		const std::string reference(input);
		if (const auto* translated = lookup(reference.c_str()); translated && *translated)
			return translated;
		return reference;
	}
}
