#include <std_include.hpp>
#include "game/game.hpp"
#include "fastfiles.hpp"
#include "xenon.hpp"
#include <zlib.h>
#include <bit>
#include <fstream>
#include <intrin.h>
#include <mutex>
#include <tuple>
#include <type_traits>

namespace fastfiles::xenon
{
	namespace
	{
		using bytes = std::vector<unsigned char>;
		constexpr uint32_t inline_data = 0xFFFFFFFFu;
		constexpr uint32_t insert_pointer = 0xFFFFFFFEu;
		constexpr size_t size_limit = 128u * 1024u * 1024u;
		constexpr uint32_t xenon_asset_count = 39;

		struct asset_entry
		{
			uint32_t xenon_type;
			uint32_t pointer;
		};

		struct zone_manifest
		{
			uint32_t script_string_count;
			std::vector<asset_entry> assets;
		};

		// QoS Xenon has a standalone pixel-shader asset at slot 7. PC does not.
		// Every later asset is consequently shifted down by one in the PC dispatcher.
		std::optional<uint32_t> pc_asset_type(const uint32_t xenon_type)
		{
			if (xenon_type >= xenon_asset_count) throw std::runtime_error("invalid Xenon asset type");
			if (xenon_type == 7) return std::nullopt;
			return xenon_type < 7 ? xenon_type : xenon_type - 1;
		}

		struct verified_layout
		{
			size_t xenon_size;
			size_t pc_size;
		};

		// Sizes recovered from the QoS Xenon and PC DB loaders. Records with unequal
		// sizes require field-wise serialization and must never be byte-swapped in place.
		constexpr verified_layout material_layout{96, 104};
		constexpr verified_layout technique_set_layout{156, 184};
		constexpr verified_layout image_layout{40, 36};
		constexpr verified_layout sound_alias_layout{96, 80};
		constexpr verified_layout xsurface_layout{200, 80};
		constexpr verified_layout gfx_world_layout{828, 728};
		constexpr verified_layout xmodel_layout{240, 240};
		constexpr verified_layout clip_map_layout{324, 324};
		constexpr verified_layout common_world_layout{44, 44};
		constexpr verified_layout game_world_layout{4, 4};
		constexpr verified_layout light_layout{16, 16};
		constexpr verified_layout fx_layout{32, 32};

		void require(const bool condition, const char* message)
		{
			if (!condition) throw std::runtime_error(message);
		}

		template <typename T>
		T byte_swap(const T value)
		{
			static_assert(std::is_integral_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
			if constexpr (sizeof(T) == 1) return value;
			if constexpr (sizeof(T) == 2) return static_cast<T>(_byteswap_ushort(static_cast<uint16_t>(value)));
			if constexpr (sizeof(T) == 4) return static_cast<T>(_byteswap_ulong(static_cast<uint32_t>(value)));
			if constexpr (sizeof(T) == 8) return static_cast<T>(_byteswap_uint64(static_cast<uint64_t>(value)));
		}

		template <typename T>
		T native(const bytes& data, const size_t offset)
		{
			require(offset <= data.size() && sizeof(T) <= data.size() - offset, "truncated fastfile value");
			T value{};
			std::memcpy(&value, data.data() + offset, sizeof(value));
			return value;
		}

		template <typename T>
		T big_endian(const bytes& data, const size_t offset)
		{
			const auto value = native<T>(data, offset);
			if constexpr (std::endian::native == std::endian::big) return value;
			return byte_swap(value);
		}

		template <typename T>
		void little_endian(bytes& data, const size_t offset, T value)
		{
			require(offset <= data.size() && sizeof(T) <= data.size() - offset, "truncated PC fastfile value");
			if constexpr (std::endian::native == std::endian::big) value = byte_swap(value);
			std::memcpy(data.data() + offset, &value, sizeof(value));
		}
		uint32_t be32(const bytes& data, const size_t offset) { return big_endian<uint32_t>(data, offset); }
		uint16_t be16(const bytes& data, const size_t offset) { return big_endian<uint16_t>(data, offset); }
		void le32(bytes& data, const size_t offset, const uint32_t value) { little_endian(data, offset, value); }
		void le16(bytes& data, const size_t offset, const uint16_t value) { little_endian(data, offset, value); }
		uint32_t native32(const bytes& data, const size_t offset) { return native<uint32_t>(data, offset); }

		uint32_t half_to_float_bits(const uint16_t value)
		{
			const auto sign = uint32_t(value & 0x8000u) << 16;
			auto exponent = uint32_t(value >> 10) & 0x1Fu;
			auto mantissa = uint32_t(value & 0x03FFu);
			if (exponent == 0)
			{
				if (!mantissa) return sign;
				exponent = 113;
				while (!(mantissa & 0x0400u))
				{
					mantissa <<= 1;
					--exponent;
				}
				return sign | (exponent << 23) | ((mantissa & 0x03FFu) << 13);
			}
			if (exponent == 0x1Fu) return sign | 0x7F800000u | (mantissa << 13);
			return sign | ((exponent + 112) << 23) | (mantissa << 13);
		}

		struct pc_surface_vertices
		{
			bytes primary;
			bytes secondary;
		};

		[[maybe_unused]] pc_surface_vertices convert_surface_vertices(const bytes& positions,
			const bytes& attributes, const std::optional<bytes>& secondary, const uint16_t vertex_count)
		{
			const auto xenon_size = size_t(vertex_count) * 16;
			require(positions.size() == xenon_size && attributes.size() == xenon_size,
				"invalid Xenon surface vertex streams");
			require(!secondary || secondary->size() == xenon_size, "invalid Xenon secondary vertex stream");
			pc_surface_vertices result{bytes(size_t(vertex_count) * 40), secondary ? bytes(xenon_size) : bytes{}};
			for (size_t i = 0; i < vertex_count; ++i)
			{
				const auto source = i * 16;
				const auto target = i * 40;
				for (size_t component = 0; component < 4; ++component)
					le32(result.primary, target + component * 4, be32(positions, source + component * 4));
				le32(result.primary, target + 16, be32(attributes, source)); // color
				le32(result.primary, target + 20, 0u); // ignored by the QoS PC vertex declaration
				le32(result.primary, target + 24, half_to_float_bits(be16(attributes, source + 8)));
				le32(result.primary, target + 28, half_to_float_bits(be16(attributes, source + 10)));
				le32(result.primary, target + 32, be32(attributes, source + 12)); // normal
				le32(result.primary, target + 36, be32(attributes, source + 4)); // tangent
				if (secondary)
				{
					for (size_t component = 0; component < 4; ++component)
						le32(result.secondary, source + component * 4, be32(*secondary, source + component * 4));
				}
			}
			return result;
		}

		template <typename... Args>
		void warn(const char* format, Args... args)
		{
			game::Com_Printf(16, format, args...);
		}

		struct reader
		{
			const bytes& data;
			size_t position = 0;

			bytes take(const size_t size)
			{
				require(position <= data.size() && size <= data.size() - position, "truncated Xenon asset");
				bytes result(data.begin() + position, data.begin() + position + size);
				position += size;
				return result;
			}

			std::string string()
			{
				const auto start = position;
				while (position < data.size() && data[position]) ++position;
				require(position < data.size() && position - start < 1024, "invalid Xenon asset name");
				std::string result(data.begin() + start, data.begin() + position);
				++position;
				return result;
			}
		};

		struct writer
		{
			bytes data;
			void append(const bytes& value) { data.insert(data.end(), value.begin(), value.end()); }
			void string(const std::string& value)
			{
				data.insert(data.end(), value.begin(), value.end());
				data.push_back(0);
			}
		};

		zone_manifest read_manifest(reader& input)
		{
			const auto list = input.take(16);
			zone_manifest result{be32(list, 0), {}};
			const auto script_string_pointer = be32(list, 4);
			const auto asset_count = be32(list, 8);
			const auto asset_pointer = be32(list, 12);
			require(result.script_string_count <= input.data.size() / 4, "invalid Xenon script-string count");
			require(asset_count <= input.data.size() / 8, "invalid Xenon asset count");
			require(!result.script_string_count || script_string_pointer == inline_data,
				"unsupported Xenon script-string array reference");
			require(!asset_count || asset_pointer == inline_data, "unsupported Xenon asset array reference");

			const auto script_pointers = input.take(size_t(result.script_string_count) * 4);
			for (uint32_t i = 0; i < result.script_string_count; ++i)
			{
				require(be32(script_pointers, size_t(i) * 4) == inline_data,
					"unsupported Xenon script-string reference");
				input.string();
			}

			const auto table = input.take(size_t(asset_count) * 8);
			result.assets.reserve(asset_count);
			for (uint32_t i = 0; i < asset_count; ++i)
			{
				const auto type = be32(table, size_t(i) * 8);
				const auto pointer = be32(table, size_t(i) * 8 + 4);
				(void)pc_asset_type(type); // Validate against the Xenon dispatcher before parsing records.
				result.assets.push_back({type, pointer});
			}
			return result;
		}

		struct image_asset
		{
			std::string name;
			uint16_t width = 0, height = 0;
			uint32_t format = 0;
			bytes pixels;
		};

		image_asset placeholder_image(std::string name, const char* reason)
		{
			warn("^3[Xenon] %s: %s; using a placeholder texture\n", name.c_str(), reason);
			image_asset result;
			result.name = std::move(name);
			result.width = 4;
			result.height = 4;
			result.format = 0x31545844u; // DXT1
			result.pixels = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00};
			return result;
		}

		// Xenos 2D address bit layout, corroborated by:
		// https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_address.h
		// Coordinates and pitch are in compression blocks. Only base mip is emitted.
		size_t tiled_offset(const size_t x, const size_t y, const size_t pitch, const unsigned int block_bytes_log2)
		{
			require((pitch & 31) == 0, "unaligned Xbox texture pitch");
			const auto outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
			const auto inner = (((y >> 1) & 7) << 3) | (x & 7);
			const auto address = (outer | inner) << block_bytes_log2;
			const auto bank = (y >> 4) & 1;
			const auto pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
			return (address & 15) | (((address >> 4) & 1) << 5) | (((address >> 5) & 7) << 8)
				| ((address >> 8) << 12) | ((y & 1) << 4) | (pipe << 6) | (bank << 11);
		}

		size_t endian_index(const size_t index, const uint32_t endian)
		{
			switch (endian)
			{
			case 0: return index;
			case 1: return index ^ 1; // Xenos k8in16
			case 2: return index ^ 3; // Xenos k8in32
			case 3: return index ^ 2; // Xenos k16in32
			default: throw std::runtime_error("invalid Xbox texture endian mode");
			}
		}

		image_asset read_image(reader& input)
		{
			// Xenon 0x821E7D60 / 0x821E7C78: image, name, GPU data, load definition, resource.
			const auto header = input.take(image_layout.xenon_size);
			require(be32(header, 36) == inline_data && be32(header, 24) == inline_data,
				"unsupported Xenon image reference");
			require(be32(header, 4) == inline_data || be32(header, 4) == insert_pointer, "unsupported image load reference");
			image_asset result;
			result.name = input.string();
			result.width = be16(header, 16);
			result.height = be16(header, 18);
			const auto gpu_data = input.take(be32(header, 12));
			const auto load = input.take(16);
			const auto resource = input.take(52);
			if (be32(load, 12) == 0)
				return placeholder_image(std::move(result.name), "Xbox image has an empty texture resource");
			if (be32(header, 0) != 3 || be16(header, 20) != 1 || (load[1] & 12))
				return placeholder_image(std::move(result.name), "unsupported Xbox image dimension or resource flags");
			if (be16(load, 2) != result.width || be16(load, 4) != result.height || be16(load, 6) != 1)
				return placeholder_image(std::move(result.name), "Xbox image dimensions disagree");
			if (result.width < 128 || result.width > 4096 || result.height < 128 || result.height > 4096)
				return placeholder_image(std::move(result.name), "unsupported Xbox image dimensions");
			const auto format = be32(load, 8);
			const auto format_without_endian = format & ~0x300u;
			if (format_without_endian != 0x1A200052 && format_without_endian != 0x1A200053)
				return placeholder_image(std::move(result.name), "unsupported Xbox texture format");
			const size_t block_bytes = format_without_endian == 0x1A200052 ? 8 : 16;
			const unsigned int block_bytes_log2 = block_bytes == 8 ? 3 : 4;
			const auto endian = (format >> 8) & 3; // Xenos Endian field.
			result.format = block_bytes == 8 ? 0x31545844u : 0x33545844u; // DXT1 / DXT3
			const size_t width = (result.width + 3u) / 4u;
			const size_t height = (result.height + 3u) / 4u;
			const size_t pitch = (width + 31u) & ~size_t(31);
			// These files have tightly specified pitch. Do not assume it for arbitrary resources.
			if (!((result.width == 1024 && result.height == 512 && be32(resource, 28) == 0x02000088)
				|| (result.width == 1360 && result.height == 768 && be32(resource, 28) == 0x0200008B)))
				return placeholder_image(std::move(result.name), "unverified Xbox texture pitch");
			result.pixels.resize(width * height * block_bytes);
			for (size_t y = 0; y < height; ++y)
			{
				for (size_t x = 0; x < width; ++x)
				{
					const auto source = tiled_offset(x, y, pitch, block_bytes_log2);
					if (source > gpu_data.size() || block_bytes > gpu_data.size() - source)
						return placeholder_image(std::move(result.name), "Xbox texture exceeds its resource");
					const auto target = (y * width + x) * block_bytes;
					for (size_t i = 0; i < block_bytes; ++i)
						result.pixels[target + i] = gpu_data[source + endian_index(i, endian)];
				}
			}
			return result;
		}

		struct material_asset
		{
			std::string name;
			bytes texture;
			image_asset image;
		};

		struct material_template
		{
			bytes header, texture, constants, states;
			std::string technique;
		};

		material_template pc_template(const std::string& name)
		{
			// Reuse PC UI shader/state definitions; Xbox shader programs are not portable.
			// Enumerate instead of requesting defaults, which may fatal or mutate the DB.
			void* exact = nullptr;
			void* white = nullptr;
			fastfiles::enum_assets(game::ASSET_TYPE_MATERIAL, [&](game::XAssetHeader asset)
			{
				if (!asset.data) return;
				const auto* candidate = *static_cast<const char**>(asset.data);
				if (!candidate) return;
				if (!_stricmp(candidate, name.c_str())) exact = asset.data;
				if (!_stricmp(candidate, "white")) white = asset.data;
			}, false);
			auto extract = [](void* asset) -> std::optional<material_template>
			{
				if (!asset) return std::nullopt;
				material_template result;
				const auto* selected = static_cast<const unsigned char*>(asset);
				result.header.assign(selected, selected + 104);
				const auto* technique = reinterpret_cast<const game::MaterialTechniqueSet*>(native32(result.header, 84));
				if (!technique || !technique->name || std::strcmp(technique->name, "2d")) return std::nullopt;
				result.technique = technique->name;
				if (result.header[67] != 1 || result.header[69] == 0) return std::nullopt;
				const auto* texture = reinterpret_cast<const unsigned char*>(native32(result.header, 88));
				if (!texture || texture[7] != 0) return std::nullopt;
				result.texture.assign(texture, texture + 12);
				for (const auto& section : {std::make_tuple(68u, 92u, 32u, &result.constants),
					std::make_tuple(69u, 96u, 8u, &result.states)})
				{
					const auto size = size_t(result.header[std::get<0>(section)]) * std::get<2>(section);
					const auto* source = reinterpret_cast<const unsigned char*>(native32(result.header, std::get<1>(section)));
					if (size && !source) return std::nullopt;
					if (size) std::get<3>(section)->assign(source, source + size);
				}
				return result;
			};
			auto result = extract(exact);
			if (!result)
			{
				if (exact) warn("^3[Xenon] %s has an incompatible PC material; using white/2d render state\n", name.c_str());
				result = extract(white);
			}
			require(result.has_value(), "PC white/2d material is unavailable; load common_mp first");
			if (!exact) warn("^3[Xenon] %s uses PC white/2d render state\n", name.c_str());
			return std::move(*result);
		}

		void write_material(writer& output, const material_asset& material)
		{
			auto base = pc_template(material.name);
			le32(base.header, 0, inline_data);
			le32(base.header, 84, inline_data);
			le32(base.header, 88, inline_data);
			le32(base.header, 92, base.constants.empty() ? 0u : inline_data);
			le32(base.header, 96, inline_data);
			output.append(base.header);
			output.string(material.name);
			bytes technique(184, 0);
			le32(technique, 0, inline_data);
			output.append(technique);
			output.string("," + base.technique); // PC 0x103E0640 resolves comma-prefixed external assets.
			if (native32(base.texture, 0) != be32(material.texture, 0))
				warn("^3[Xenon] %s has a different Xbox texture semantic; keeping the PC template semantic\n", material.name.c_str());
			auto texture = base.texture;
			le32(texture, 8, inline_data);
			output.append(texture);
			bytes image(image_layout.pc_size, 0);
			le32(image, 0, 3);
			le32(image, 4, inline_data);
			image[10] = 1; // no picmip; UI keeps the original base-level resolution
			le16(image, 24, material.image.width);
			le16(image, 26, material.image.height);
			le16(image, 28, 1);
			image[30] = 3;
			le32(image, 32, inline_data);
			output.append(image);
			output.string(material.image.name);
			bytes load(16, 0);
			load[0] = 1;
			load[1] = 3; // no picmip, no mip chain: only the verified Xbox base level is converted
			le16(load, 2, material.image.width);
			le16(load, 4, material.image.height);
			le16(load, 6, 1);
			le32(load, 8, material.image.format);
			le32(load, 12, static_cast<uint32_t>(material.image.pixels.size()));
			output.append(load);
			output.append(material.image.pixels);
			output.append(base.constants);
			output.append(base.states);
		}

		bytes convert(const bytes& file)
		{
			// Verified against QoS Xenon 0x821E15F8 and PC 0x103DDFF0 (both v470).
			require(file.size() >= 28, "truncated Xenon header");
			const auto expected = be32(file, 4);
			require(expected >= 16 && expected <= size_limit, "invalid Xenon decompressed size");
			bytes payload(expected);
			uLongf size = expected;
			const auto status = uncompress(payload.data(), &size, file.data() + 28, static_cast<uLong>(file.size() - 28));
			require(status == Z_OK && size == expected, "Xenon decompression failed or size mismatch");
			reader input{payload};
			const auto manifest = read_manifest(input);
			require(manifest.script_string_count == 0 && manifest.assets.size() == 5,
				"unsupported Xenon zone: map serialization is not complete");
			constexpr uint32_t kinds[] = {8, 6, 6, 6, 33};
			for (size_t i = 0; i < 5; ++i)
			{
				require(manifest.assets[i].xenon_type == kinds[i] && manifest.assets[i].pointer == inline_data,
					"unsupported Xenon loading-zone asset table");
			}
			const auto technique = input.take(technique_set_layout.xenon_size);
			require(be32(technique, 0) == inline_data
				&& std::all_of(technique.begin() + 4, technique.end(), [](auto b) { return b == 0; }),
				"Xbox shader conversion is not implemented");
			const auto technique_name = input.string();
			if (technique_name != ",2d")
				warn("^3[Xenon] Xbox technique %s is replaced by the PC 2d technique\n", technique_name.c_str());
			std::vector<material_asset> materials;
			for (size_t i = 0; i < 3; ++i)
			{
				const auto header = input.take(material_layout.xenon_size);
				require(be32(header, 0) == inline_data && header[60] == 1 && header[61] == 0 && header[62] == 1
					&& be32(header, 76) == 0x40000005 && be32(header, 80) == inline_data
					&& be32(header, 84) == 0 && be32(header, 88) == inline_data, "unsupported Xbox UI material layout");
				material_asset material;
				material.name = input.string();
				constexpr const char* names[] = {"$victorybackdrop", "$defeatbackdrop", "$levelbriefing"};
				if (material.name != names[i])
					warn("^3[Xenon] Unexpected loading-screen material %s in slot %u\n",
						material.name.c_str(), static_cast<unsigned int>(i));
				material.texture = input.take(12);
				require(be32(material.texture, 8) == inline_data, "unsupported UI texture reference");
				if (material.texture[7] != 0)
					warn("^3[Xenon] %s uses a non-color Xbox texture semantic; keeping the PC color semantic\n", material.name.c_str());
				material.image = read_image(input);
				input.take(8); // Xbox state bits are replaced by the matching native PC UI state.
				materials.push_back(std::move(material));
			}
			auto raw = input.take(12);
			require(be32(raw, 0) == inline_data && be32(raw, 8) == inline_data, "unsupported rawfile reference");
			const auto name = input.string();
			const auto length = be32(raw, 4);
			require(length < size_limit, "invalid rawfile length");
			const auto contents = input.take(size_t(length) + 1);
			require(contents.back() == 0 && input.position == payload.size(), "Xenon parser did not consume the exact asset stream");
			writer output;
			bytes pc_list(16 + 4 * 8, 0);
			le32(pc_list, 8, 4);
			le32(pc_list, 12, inline_data);
			for (size_t i = 0; i < 4; ++i)
			{
				const auto converted_type = pc_asset_type(manifest.assets[i + 1].xenon_type);
				require(converted_type.has_value(), "Xenon pixel shaders cannot be emitted as PC assets");
				le32(pc_list, 16 + i * 8, *converted_type);
				le32(pc_list, 20 + i * 8, inline_data);
			}
			output.append(pc_list);
			for (const auto& material : materials) write_material(output, material);
			le32(raw, 0, inline_data);
			le32(raw, 4, length);
			le32(raw, 8, inline_data);
			output.append(raw);
			output.string(name);
			output.append(contents);
			bytes result(28, 0);
			le32(result, 0, 470);
			le32(result, 4, static_cast<uint32_t>(output.data.size()));
			// Conservative bounds cover temporary records and aligned persistent data.
			const auto allocation = static_cast<uint32_t>(output.data.size() + 65536);
			le32(result, 8, allocation);
			le32(result, 16, allocation);
			uLongf compressed_size = compressBound(static_cast<uLong>(output.data.size()));
			result.resize(28 + compressed_size);
			require(compress2(result.data() + 28, &compressed_size, output.data.data(),
				static_cast<uLong>(output.data.size()), Z_BEST_SPEED) == Z_OK, "PC zone compression failed");
			// Native PC and Xenon QoS fastfiles pad the zlib stream to 32 bytes.
			result.resize((28 + compressed_size + 31) & ~size_t(31), 0);
			return result;
		}

		struct cached_file
		{
			std::wstring path;
			HANDLE keeper = INVALID_HANDLE_VALUE;
			~cached_file() { if (keeper != INVALID_HANDLE_VALUE) CloseHandle(keeper); }
		};
		std::mutex cache_mutex;
		std::unordered_map<std::string, std::shared_ptr<cached_file>> cache;

		std::string key(std::string name)
		{
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name;
		}
	}

	bool prepare(const std::filesystem::path& source, const std::string& zone_name, const bool allow_pc_probe)
	{
		std::ifstream stream(source, std::ios::binary | std::ios::ate);
		require(stream.good(), "cannot open fastfile source");
		const auto length = stream.tellg();
		require(length >= 4 && length <= static_cast<std::streamoff>(size_limit), "invalid fastfile source size");
		stream.seekg(0);
		bytes signature(4);
		stream.read(reinterpret_cast<char*>(signature.data()), 4);
		if (native32(signature, 0) == 470)
		{
			if (!allow_pc_probe) return false;
			auto entry = std::make_shared<cached_file>();
			entry->path = std::filesystem::absolute(source).wstring();
			entry->keeper = CreateFileW(entry->path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			require(entry->keeper != INVALID_HANDLE_VALUE, "cannot retain PC conversion probe");
			{
				std::lock_guard lock(cache_mutex);
				cache[key(zone_name)] = std::move(entry);
			}
			game::Com_Printf(16, "^5[Xenon] Prepared explicit PC v470 conversion probe %s\n", zone_name.c_str());
			return true;
		}
		if (be32(signature, 0) != 470)
		{
			require(signature[0] != 0 || signature[1] != 0 || be32(signature, 0) == 0,
				"unrecognized big-endian fastfile version; no conversion schema is available");
			return false;
		}
		bytes file(static_cast<size_t>(length));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
		require(stream.good(), "fastfile source read failed");
		game::DB_WaitXAssets.get()();
		const auto converted = convert(file);
		wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
		const auto directory_size = GetTempPathW(MAX_PATH, directory);
		require(directory_size && directory_size < MAX_PATH && GetTempFileNameW(directory, L"qff", 0, path), "cannot create converted-zone temporary file");
		const auto writer_handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
		if (writer_handle == INVALID_HANDLE_VALUE) { DeleteFileW(path); throw std::runtime_error("cannot write converted zone"); }
		DWORD written = 0;
		const auto wrote = WriteFile(writer_handle, converted.data(), static_cast<DWORD>(converted.size()), &written, nullptr);
		CloseHandle(writer_handle);
		if (!wrote || written != converted.size()) { DeleteFileW(path); throw std::runtime_error("converted zone write failed"); }
		auto entry = std::make_shared<cached_file>();
		entry->path = path;
		entry->keeper = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		if (entry->keeper == INVALID_HANDLE_VALUE) { DeleteFileW(path); throw std::runtime_error("cannot retain converted zone"); }
		{
			std::lock_guard lock(cache_mutex);
			cache[key(zone_name)] = std::move(entry);
		}
		game::Com_Printf(16, "^5[Xenon] Prepared %s: converted to PC v470\n", zone_name.c_str());
		return true;
	}

	std::optional<HANDLE> open_prepared(const char* file_name, const DWORD access, const DWORD sharing,
		LPSECURITY_ATTRIBUTES security, const DWORD disposition, const DWORD flags, HANDLE template_file)
	{
		std::shared_ptr<cached_file> entry;
		{
			std::lock_guard lock(cache_mutex);
			const auto found = cache.find(key(std::filesystem::path(file_name).stem().string()));
			if (found == cache.end()) return std::nullopt;
			entry = found->second;
		}
		// The cache's keeper has GENERIC_READ access. A new open must permit that
		// existing read handle to remain shared, even if the engine requested 0.
		return CreateFileW(entry->path.c_str(), access,
			sharing | FILE_SHARE_READ | FILE_SHARE_DELETE, security, disposition, flags, template_file);
	}

	void clear()
	{
		std::lock_guard lock(cache_mutex);
		cache.clear();
	}
}
