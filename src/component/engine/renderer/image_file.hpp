#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>

namespace image_overrides
{
	inline constexpr std::size_t maximum_file_size = 64 * 1024 * 1024;
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

	inline std::uint32_t read_u32(const std::span<const unsigned char> data, const std::size_t offset)
	{
		if (offset > data.size() || data.size() - offset < 4)
			throw std::runtime_error("truncated image header");
		return data[offset] | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(data[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(data[offset + 3]) << 24);
	}

	inline std::vector<unsigned char> make_bgra_definition(const unsigned width, const unsigned height,
		const std::span<const unsigned char> pixels)
	{
		if (!width || !height || width > 4096 || height > 4096
			|| pixels.size() != static_cast<std::size_t>(width) * height * 4
			|| pixels.size() > maximum_file_size)
			throw std::runtime_error("invalid decoded BGRA dimensions/payload");
		// QoS 0x10381D20/0x10381B60 support D3DFMT_A8R8G8B8 (21).
		const load_definition header{1, 3, static_cast<std::uint16_t>(width),
			static_cast<std::uint16_t>(height), 1, 21, static_cast<std::uint32_t>(pixels.size())};
		std::vector<unsigned char> result(sizeof(header) + pixels.size());
		std::memcpy(result.data(), &header, sizeof(header));
		std::memcpy(result.data() + sizeof(header), pixels.data(), pixels.size());
		return result;
	}

	inline std::vector<unsigned char> decode_dds(const std::span<const unsigned char> file)
	{
		// Microsoft DDS_HEADER/DDS_PIXELFORMAT offsets. No packed C++ ABI dependency.
		if (file.size() < 128 || file.size() > maximum_file_size
			|| read_u32(file, 0) != 0x20534444 || read_u32(file, 4) != 124
			|| read_u32(file, 76) != 32)
			throw std::runtime_error("invalid DDS header/size");
		const unsigned width = read_u32(file, 16), height = read_u32(file, 12);
		const unsigned caps2 = read_u32(file, 112);
		if (!width || !height || width > 4096 || height > 4096
			|| read_u32(file, 24) > 1 || (caps2 & 0x200000))
			throw std::runtime_error("DDS volumes/invalid dimensions are unsupported");
		bool cube = (caps2 & 0x200) != 0;
		if ((cube && (caps2 & 0xFC00) != 0xFC00) || (!cube && (caps2 & 0xFC00)))
			throw std::runtime_error("DDS requires all six cubemap faces");
		unsigned format = 0;
		bool rgba = false, opaque = false;
		std::size_t start = 128;
		if (read_u32(file, 80) & 4)
		{
			format = read_u32(file, 84);
			if (format == 0x30315844) // DX10
			{
				if (file.size() < 148 || read_u32(file, 132) != 3 || read_u32(file, 140) != 1
					|| (read_u32(file, 136) & ~4u) || read_u32(file, 144) > 3)
					throw std::runtime_error("unsupported DDS DX10 resource/array/alpha mode");
				if (read_u32(file, 144) == 2)
					throw std::runtime_error("premultiplied DDS alpha is unsupported");
				cube = (read_u32(file, 136) & 4) != 0;
				opaque = read_u32(file, 144) == 3;
				start = 148;
				switch (read_u32(file, 128))
				{
				case 71: format = 0x31545844; break; // BC1_UNORM
				case 74: format = 0x33545844; break; // BC2_UNORM
				case 77: format = 0x35545844; break; // BC3_UNORM
				case 28: format = 21; rgba = true; break; // R8G8B8A8_UNORM
				case 87: format = 21; break; // B8G8R8A8_UNORM
				case 88: format = 21; opaque = true; break;
				default: throw std::runtime_error("unsupported DDS DXGI format (BC1/2/3 or RGBA/BGRA UNORM required)");
				}
			}
			else if (format != 0x31545844 && format != 0x33545844 && format != 0x35545844)
				throw std::runtime_error("unsupported DDS FourCC (DXT1/3/5 required)");
		}
		else
		{
			if (!(read_u32(file, 80) & 0x40) || read_u32(file, 88) != 32
				|| read_u32(file, 96) != 0xFF00
				|| (read_u32(file, 104) != 0xFF000000 && read_u32(file, 104) != 0))
				throw std::runtime_error("unsupported DDS RGB masks/bit depth");
			const auto red = read_u32(file, 92), blue = read_u32(file, 100);
			rgba = red == 0xFF && blue == 0xFF0000;
			if (!rgba && !(red == 0xFF0000 && blue == 0xFF))
				throw std::runtime_error("unsupported DDS channel masks");
			opaque = read_u32(file, 104) == 0;
			format = 21;
		}
		if (cube && width != height) throw std::runtime_error("DDS cubemap must be square");
		const unsigned levels = std::max(1u, read_u32(file, 28));
		unsigned full_levels = 1;
		for (unsigned size = std::max(width, height); size > 1; size >>= 1) ++full_levels;
		// QoS derives upload mip count from dimensions, not level_count. A
		// partial chain would make it read beyond our validated buffer.
		if (levels != 1 && (levels != full_levels || (width & (width - 1)) || (height & (height - 1))))
			throw std::runtime_error("DDS needs a complete power-of-two mip chain or a single level");
		std::vector<std::size_t> sizes, offsets;
		std::size_t face_size = 0;
		for (unsigned level = 0; level < levels; ++level)
		{
			const auto w = std::max(1u, width >> level), h = std::max(1u, height >> level);
			const std::size_t size = format == 21 ? static_cast<std::size_t>(w) * h * 4
				: static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * (format == 0x31545844 ? 8 : 16);
			offsets.push_back(face_size); sizes.push_back(size); face_size += size;
		}
		const unsigned faces = cube ? 6 : 1;
		const auto total = face_size * faces;
		if (total != file.size() - start) throw std::runtime_error("DDS payload/mip size mismatch (tightly packed data required)");
		if (format == 21 && (read_u32(file, 8) & 8) && read_u32(file, 20) != width * 4)
			throw std::runtime_error("padded DDS rows are unsupported");
		const load_definition header{static_cast<std::uint8_t>(levels),
			static_cast<std::uint8_t>(1 | (levels == 1 ? 2 : 0) | (cube ? 4 : 0)),
			static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), 1, format,
			static_cast<std::uint32_t>(total)};
		std::vector<unsigned char> result(sizeof(header) + total);
		std::memcpy(result.data(), &header, sizeof(header));
		std::size_t destination = sizeof(header);
		// DDS: face-major, largest mip first. QoS: mip-major, smallest first.
		for (unsigned level = levels; level-- > 0;)
			for (unsigned face = 0; face < faces; ++face)
			{
				const auto source = start + face * face_size + offsets[level];
				std::memcpy(result.data() + destination, file.data() + source, sizes[level]);
				if (format == 21)
					for (std::size_t p = destination; p < destination + sizes[level]; p += 4)
					{
						if (rgba) std::swap(result[p], result[p + 2]);
						if (opaque) result[p + 3] = 255;
					}
				destination += sizes[level];
			}
		return result;
	}

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
				// Windows device basenames remain special even with an extension.
				const auto base = part.substr(0, part.find('.'));
				char upper[5]{};
				if (base.size() <= 4)
				{
					for (std::size_t c = 0; c < base.size(); ++c)
						upper[c] = base[c] >= 'a' && base[c] <= 'z' ? base[c] - 'a' + 'A' : base[c];
					const std::string_view device(upper, base.size());
					if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL"
						|| (device.size() == 4 && (device.substr(0, 3) == "COM" || device.substr(0, 3) == "LPT")
							&& device[3] >= '1' && device[3] <= '9')) return false;
				}
				start = i + 1;
			}
			else if (static_cast<unsigned char>(name[i]) < 32
				|| std::string_view(":*?\"<>|").find(name[i]) != std::string_view::npos)
				return false;
		}
		return true;
	}

	inline std::string image_filename(const std::string_view name)
	{
		// Percent-encode Windows-illegal asset characters (e.g. *lightmap),
		// including '%' itself to keep names collision-free. Keep directories.
		constexpr char hex[] = "0123456789ABCDEF";
		std::string result;
		for (const unsigned char c : name)
		{
			if (c < 32 || c >= 127 || std::string_view(":*?\"<>|%").find(c) != std::string_view::npos)
			{
				result += '%'; result += hex[c >> 4]; result += hex[c & 15];
			}
			else result += static_cast<char>(c == '\\' ? '/' : c);
		}
		if (!safe_image_name(result)) throw std::runtime_error("unsafe image asset path");
		return result;
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
