#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace image_overrides
{
	// IWI v6 header/format values are from KisakCOD r_image.h. QoS does not
	// expose that loose-file reader: translate only validated compressed data
	// into its native 16-byte load definition (PC loader 0x103D1D40).
	struct load_definition
	{
		std::uint8_t level_count;
		std::uint8_t flags;
		std::uint16_t width, height, depth;
		std::uint32_t format;
		std::uint32_t resource_size;
	};
	static_assert(sizeof(load_definition) == 16);
	static_assert(offsetof(load_definition, resource_size) == 12);

	inline bool safe_image_name(const std::string_view name)
	{
		if (name.empty() || name.size() > 240 || name.front() == ','
			|| name.front() == '/' || name.front() == '\\') return false;
		std::size_t start = 0;
		for (std::size_t i = 0; i <= name.size(); ++i)
		{
			if (i == name.size() || name[i] == '/' || name[i] == '\\')
			{
				const auto part = name.substr(start, i - start);
				if (part.empty() || part == "." || part == ".."
					|| part.back() == '.' || part.back() == ' ') return false;
				start = i + 1;
			}
			else if (static_cast<unsigned char>(name[i]) < 32
				|| std::string_view(":*?\"<>|").find(name[i]) != std::string_view::npos)
				return false;
		}
		return true;
	}

	inline std::vector<unsigned char> decode_iwi(const std::span<const unsigned char> file)
	{
		constexpr std::size_t header_size = 28;
		constexpr std::size_t maximum_size = 64 * 1024 * 1024;
		if (file.size() < header_size || file.size() > maximum_size)
			throw std::runtime_error("invalid IWI file size");
		if (file[0] != 'I' || file[1] != 'W' || file[2] != 'i' || file[3] != 6)
			throw std::runtime_error("expected PC IWI version 6");
		auto read16 = [&](const std::size_t offset) -> std::uint16_t
		{
			return static_cast<std::uint16_t>(file[offset] | (file[offset + 1] << 8));
		};
		auto read32 = [&](const std::size_t offset) -> std::uint32_t
		{
			return static_cast<std::uint32_t>(file[offset])
				| (static_cast<std::uint32_t>(file[offset + 1]) << 8)
				| (static_cast<std::uint32_t>(file[offset + 2]) << 16)
				| (static_cast<std::uint32_t>(file[offset + 3]) << 24);
		};
		load_definition definition{};
		definition.width = read16(6);
		definition.height = read16(8);
		definition.depth = read16(10);
		if (!definition.width || !definition.height || definition.width > 4096
			|| definition.height > 4096 || definition.depth != 1)
			throw std::runtime_error("unsupported IWI dimensions (2D/cube, up to 4096)");
		// Reject volume, streaming, legacy-normal and unknown flags instead of
		// pretending their payload/encoding contract is supported.
		if (file[5] & ~0xC7u) throw std::runtime_error("unsupported IWI flags");
		definition.flags = file[5] & 7;
		if (!(definition.flags & 2)
			&& ((definition.width & (definition.width - 1))
				|| (definition.height & (definition.height - 1))))
			throw std::runtime_error("mipped IWI dimensions must be powers of two");
		const bool cube = (definition.flags & 4) != 0;
		if (cube && definition.width != definition.height)
			throw std::runtime_error("IWI cubemap must be square");
		const auto format = file[4];
		if (format < 11 || format > 13)
			throw std::runtime_error("only DXT1/DXT3/DXT5 IWI files are supported");
		definition.format = 0x31545844u + (static_cast<std::uint32_t>(format - 11) * 2u << 24);
		const std::size_t block_size = format == 11 ? 8 : 16;
		std::vector<std::size_t> level_sizes;
		auto width = definition.width;
		auto height = definition.height;
		do
		{
			level_sizes.push_back(static_cast<std::size_t>((width + 3) / 4)
				* ((height + 3) / 4) * block_size * (cube ? 6 : 1));
			width = std::max<std::uint16_t>(1, width / 2);
			height = std::max<std::uint16_t>(1, height / 2);
		} while (!(definition.flags & 2) && (width != 1 || height != 1));
		// The loop ends after reducing to 1x1; include that final stored mip.
		if (!(definition.flags & 2) && (definition.width != 1 || definition.height != 1))
			level_sizes.push_back(block_size * (cube ? 6 : 1));
		std::size_t total = 0;
		for (const auto size : level_sizes) total += size;
		if (total != file.size() - header_size || read32(12) != file.size())
			throw std::runtime_error("IWI mip payload size does not match header/dimensions");
		for (std::size_t picmip = 0; picmip < 4; ++picmip)
		{
			std::size_t expected = header_size;
			for (std::size_t level = std::min(picmip, level_sizes.size() - 1);
				level < level_sizes.size(); ++level) expected += level_sizes[level];
			if (read32(12 + picmip * 4) != expected)
				throw std::runtime_error("invalid IWI picmip size table");
		}
		definition.level_count = static_cast<std::uint8_t>(level_sizes.size());
		definition.resource_size = static_cast<std::uint32_t>(total);
		std::vector<unsigned char> result(sizeof(definition) + total);
		std::memcpy(result.data(), &definition, sizeof(definition));
		// Both IWI v6 and QoS PC load definitions store smallest mip first,
		// with all faces of a mip adjacent. No pixel/endian transformation.
		std::memcpy(result.data() + sizeof(definition), file.data() + header_size, total);
		return result;
	}
}
