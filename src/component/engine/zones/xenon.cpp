#include <std_include.hpp>
#include "game/game.hpp"
#include "fastfiles.hpp"
#include "xenon.hpp"
#include <zlib.h>
#include <fstream>
#include <mutex>
#include <tuple>

namespace fastfiles::xenon
{
	namespace
	{
		using bytes = std::vector<unsigned char>;
		constexpr uint32_t inline_data = 0xFFFFFFFFu;
		constexpr uint32_t insert_pointer = 0xFFFFFFFEu;
		constexpr size_t size_limit = 128u * 1024u * 1024u;

		void require(const bool condition, const char* message)
		{
			if (!condition) throw std::runtime_error(message);
		}

		uint32_t be32(const bytes& data, const size_t offset)
		{
			require(offset <= data.size() && data.size() - offset >= 4, "truncated Xenon word");
			return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16)
				| (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
		}

		uint16_t be16(const bytes& data, const size_t offset)
		{
			require(offset <= data.size() && data.size() - offset >= 2, "truncated Xenon short");
			return static_cast<uint16_t>((uint32_t(data[offset]) << 8) | data[offset + 1]);
		}

		void le32(bytes& data, const size_t offset, const uint32_t value)
		{
			for (size_t i = 0; i < 4; ++i) data.at(offset + i) = static_cast<unsigned char>(value >> (i * 8));
		}

		void le16(bytes& data, const size_t offset, const uint16_t value)
		{
			data.at(offset) = static_cast<unsigned char>(value);
			data.at(offset + 1) = static_cast<unsigned char>(value >> 8);
		}

		uint32_t native32(const bytes& data, const size_t offset)
		{
			uint32_t value;
			std::memcpy(&value, data.data() + offset, sizeof(value));
			return value;
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

		struct image_asset
		{
			std::string name;
			uint16_t width = 0, height = 0;
			uint32_t format = 0;
			bytes pixels;
		};

		// Xenos 2D address bit layout, corroborated by:
		// https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_address.h
		// Coordinates and pitch are in compression blocks. Only base mip is emitted.
		size_t tiled_offset(const size_t x, const size_t y, const size_t pitch, const size_t block_bytes)
		{
			const auto macro = ((y / 32) * (pitch / 32) + x / 32) * 64;
			const auto micro = ((y / 2) % 8) * 8 + x % 8;
			const auto address = (macro + micro) * block_bytes;
			const auto pipe = ((x / 8) % 4) ^ (((y / 8) % 2) * 2);
			return (address & 15) | ((address & 16) << 1) | ((address & 224) << 3)
				| ((address & ~size_t(255)) << 4) | ((y & 1) << 4) | (pipe << 6)
				| (((y / 16) & 1) << 11);
		}

		image_asset read_image(reader& input)
		{
			// Xenon 0x821E7D60 / 0x821E7C78: image, name, GPU data, load definition, resource.
			const auto header = input.take(40);
			require(be32(header, 0) == 3 && be16(header, 20) == 1, "only 2D Xenon UI images are supported");
			require(be32(header, 36) == inline_data && be32(header, 24) == inline_data,
				"unsupported Xenon image reference");
			require(be32(header, 4) == inline_data || be32(header, 4) == insert_pointer, "unsupported image load reference");
			image_asset result;
			result.name = input.string();
			result.width = be16(header, 16);
			result.height = be16(header, 18);
			require(result.width >= 128 && result.width <= 4096 && result.height >= 128 && result.height <= 4096,
				"unsupported Xenon image dimensions");
			const auto gpu_data = input.take(be32(header, 12));
			const auto load = input.take(16);
			require(be16(load, 2) == result.width && be16(load, 4) == result.height && be16(load, 6) == 1,
				"Xenon image dimensions disagree");
			require(be32(load, 12) != 0 && !(load[1] & 12), "unsupported Xbox texture resource");
			const auto resource = input.take(52);
			const auto format = be32(load, 8);
			require(format == 0x1A200152 || format == 0x1A200153, "unsupported Xbox texture format");
			const size_t block_bytes = format == 0x1A200152 ? 8 : 16;
			result.format = format == 0x1A200152 ? 0x31545844u : 0x33545844u; // DXT1 / DXT3
			const size_t width = (result.width + 3u) / 4u;
			const size_t height = (result.height + 3u) / 4u;
			const size_t pitch = (width + 31u) & ~size_t(31);
			// These files have tightly specified pitch. Do not assume it for arbitrary resources.
			require((result.width == 1024 && result.height == 512 && be32(resource, 28) == 0x02000088)
				|| (result.width == 1360 && result.height == 768 && be32(resource, 28) == 0x0200008B),
				"unverified Xbox texture pitch");
			result.pixels.resize(width * height * block_bytes);
			for (size_t y = 0; y < height; ++y)
			{
				for (size_t x = 0; x < width; ++x)
				{
					const auto source = tiled_offset(x, y, pitch, block_bytes);
					require(source <= gpu_data.size() && block_bytes <= gpu_data.size() - source, "Xbox texture exceeds its resource");
					const auto target = (y * width + x) * block_bytes;
					for (size_t i = 0; i < block_bytes; ++i) result.pixels[target + i] = gpu_data[source + (i ^ 1)];
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
			auto* selected = static_cast<unsigned char*>(exact ? exact : white);
			require(selected != nullptr, "PC UI material is unavailable; load common_mp first");
			material_template result;
			result.header.assign(selected, selected + 104);
			const auto* technique = reinterpret_cast<const game::MaterialTechniqueSet*>(native32(result.header, 84));
			require(technique && technique->name && !std::strcmp(technique->name, "2d"), "PC material does not use the verified 2d technique");
			result.technique = technique->name;
			require(result.header[67] == 1 && result.header[69] != 0, "unsupported PC UI material template");
			const auto* texture = reinterpret_cast<const unsigned char*>(native32(result.header, 88));
			require(texture != nullptr && texture[7] == 0, "PC template has no color texture");
			result.texture.assign(texture, texture + 12);
			for (const auto& section : {std::make_tuple(68u, 92u, 32u, &result.constants),
				std::make_tuple(69u, 96u, 8u, &result.states)})
			{
				const auto size = size_t(result.header[std::get<0>(section)]) * std::get<2>(section);
				const auto* source = reinterpret_cast<const unsigned char*>(native32(result.header, std::get<1>(section)));
				require(!size || source, "invalid PC UI template data");
				if (size) std::get<3>(section)->assign(source, source + size);
			}
			if (!exact) game::Com_Printf(16, "^3[Xenon] %s uses PC white/2d render state\n", name.c_str());
			return result;
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
			require(native32(base.texture, 0) == be32(material.texture, 0), "PC and Xbox UI texture semantics differ");
			auto texture = base.texture;
			le32(texture, 8, inline_data);
			output.append(texture);
			bytes image(36, 0);
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
			const auto list = input.take(16);
			require(be32(list, 0) == 0 && be32(list, 4) == 0 && be32(list, 8) == 5
				&& be32(list, 12) == inline_data, "unsupported Xenon zone: only the five-asset UI loading profile is implemented; map worlds are not supported");
			const auto table = input.take(40);
			constexpr uint32_t kinds[] = {8, 6, 6, 6, 33};
			for (size_t i = 0; i < 5; ++i)
			{
				require(be32(table, i * 8) == kinds[i] && be32(table, i * 8 + 4) == inline_data,
					"unsupported Xenon loading-zone asset table");
			}
			const auto technique = input.take(156);
			require(be32(technique, 0) == inline_data && std::all_of(technique.begin() + 4, technique.end(), [](auto b) { return b == 0; })
				&& input.string() == ",2d", "Xbox shader conversion is not implemented");
			std::vector<material_asset> materials;
			for (size_t i = 0; i < 3; ++i)
			{
				const auto header = input.take(96);
				require(be32(header, 0) == inline_data && header[60] == 1 && header[61] == 0 && header[62] == 1
					&& be32(header, 76) == 0x40000005 && be32(header, 80) == inline_data
					&& be32(header, 84) == 0 && be32(header, 88) == inline_data, "unsupported Xbox UI material layout");
				material_asset material;
				material.name = input.string();
				constexpr const char* names[] = {"$victorybackdrop", "$defeatbackdrop", "$levelbriefing"};
				require(material.name == names[i], "unverified Xbox loading-screen material");
				material.texture = input.take(12);
				require(material.texture[7] == 0 && be32(material.texture, 8) == inline_data, "unsupported UI texture reference");
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
				le32(pc_list, 16 + i * 8, i < 3 ? 6u : 32u);
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
			result.resize((28 + compressed_size + 0x1FFFF) & ~size_t(0x1FFFF), 0);
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

	bool prepare(const std::filesystem::path& source, const std::string& zone_name)
	{
		std::ifstream stream(source, std::ios::binary | std::ios::ate);
		require(stream.good(), "cannot open fastfile source");
		const auto length = stream.tellg();
		require(length >= 4 && length <= static_cast<std::streamoff>(size_limit), "invalid fastfile source size");
		stream.seekg(0);
		bytes signature(4);
		stream.read(reinterpret_cast<char*>(signature.data()), 4);
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
		game::Com_Printf(16, "^5[Xenon] Prepared %s: 3 UI materials, 3 base-mip textures and 1 rawfile; PC v470\n", zone_name.c_str());
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
		return CreateFileW(entry->path.c_str(), access, sharing | FILE_SHARE_DELETE, security, disposition, flags, template_file);
	}

	void clear()
	{
		std::lock_guard lock(cache_mutex);
		cache.clear();
	}
}
