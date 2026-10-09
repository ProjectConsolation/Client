#pragma once

#include <cstdint>
#include <type_traits>

namespace utils::hook::detail
{
	// x86 EIP arithmetic wraps at 32 bits: every x86 address is rel32-reachable.
	// Keep the signed displacement limit on wider hosts without signed overflow.
	template <typename Address>
	constexpr bool relative_branch_reachable(Address source, Address target, int offset = 5)
	{
		static_assert(std::is_unsigned_v<Address>);
		static_assert(sizeof(Address) == 4 || sizeof(Address) == 8);
		if constexpr (sizeof(Address) == 4)
		{
			return true;
		}
		else
		{
			const Address next = source + static_cast<Address>(offset);
			return target >= next ? target - next <= 0x7FFFFFFFull
				: next - target <= 0x80000000ull;
		}
	}

	template <typename Address>
	constexpr std::uint32_t relative_branch_bits(Address source, Address target, int offset = 5)
	{
		// Unsigned subtraction gives the exact two's-complement instruction bits.
		return static_cast<std::uint32_t>(target - (source + static_cast<Address>(offset)));
	}
}
