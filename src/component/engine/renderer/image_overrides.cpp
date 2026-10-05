#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/nt.hpp"
#include "image_file.hpp"
#include <fstream>

namespace image_overrides
{
	namespace
	{
		// Layout adapted from KisakCOD GfxImage; each accessed field verified in
		// QoS PC 1.1 0x103D3610/0x103ADD20/0x103ADC00. The texture union is a
		// loadDef before upload and a native D3D resource afterward.
		struct image_view
		{
			std::uint32_t map_type;
			void* texture;
			unsigned char picmip[2];
			unsigned char no_picmip, semantic;
			unsigned char track, padding[3];
			std::uint32_t card_memory[2];
			std::uint16_t width, height, depth;
			unsigned char category, delay_load;
			const char* name;
		};
		static_assert(sizeof(image_view) == 36);
		static_assert(offsetof(image_view, texture) == 4);
		static_assert(offsetof(image_view, semantic) == 11);
		static_assert(offsetof(image_view, width) == 24);
		static_assert(offsetof(image_view, name) == 32);

		utils::hook::detour upload_hook;
		std::filesystem::path images_directory;

		bool capture_image(image_view** image, void*** slot, char* name, const std::size_t capacity)
		{
			__try
			{
				*image = *reinterpret_cast<image_view**>(game::game_offset(0x10AB8E00));
				*slot = *reinterpret_cast<void***>(game::game_offset(0x10AB8F58));
				if (!*image || !*slot || *slot != &(*image)->texture || !**slot
					|| !(*image)->name || ((*image)->map_type != 3 && (*image)->map_type != 5)
					|| ((*image)->category != 2 && (*image)->category != 3 && (*image)->category != 4))
					return false;
				for (std::size_t i = 0; i < capacity; ++i)
				{
					name[i] = (*image)->name[i];
					if (!name[i]) return i != 0;
				}
				return false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		int upload_stub()
		{
			image_view* image = nullptr;
			void** slot = nullptr;
			char name[241]{};
			std::vector<unsigned char> definition;
			std::filesystem::path path;
			std::string display_path;
			bool overridden = false;
			// Only preparation is recoverable. Do not catch an engine/D3D failure
			// and call upload a second time against a partially initialized image.
			try
			{
				if (capture_image(&image, &slot, name, sizeof(name)) && safe_image_name(name))
				{
					path = images_directory / (std::string(name) + ".iwi");
					std::error_code error;
					if (std::filesystem::is_regular_file(path, error))
					{
						const auto size = std::filesystem::file_size(path);
						if (size > 64 * 1024 * 1024 || size < 28)
							throw std::runtime_error("invalid IWI file size");
						std::ifstream file(path, std::ios::binary);
						std::vector<unsigned char> data(static_cast<std::size_t>(size));
						if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
							throw std::runtime_error("could not read complete IWI override");
						definition = decode_iwi(data);
						load_definition header{};
						std::memcpy(&header, definition.data(), sizeof(header));
						if (((header.flags & 4) ? 5u : 3u) != image->map_type)
							throw std::runtime_error("IWI texture type differs from zone image");
						display_path = path.string();
						// All validation precedes mutation. Native upload consumes this
						// buffer synchronously and replaces *slot with its D3D texture.
						image->width = header.width;
						image->height = header.height;
						image->depth = header.depth;
						// A complete explicit mip chain bypasses native picmip. Keep
						// accounting consistent with the full-resolution replacement.
						image->no_picmip = 1;
						image->picmip[0] = image->picmip[1] = 0;
						image->card_memory[0] = image->card_memory[1] = header.resource_size;
						*slot = definition.data();
						overridden = true;
					}
				}
			}
			catch (const std::exception& error)
			{
				game::Com_Printf(16, "^3[images] Ignoring override '%s': %s\n", name, error.what());
			}
			const auto result = upload_hook.invoke<int>();
			if (overridden)
				game::Com_Printf(16, "^5[images] Loaded override '%s' from %s\n", name, display_path.c_str());
			// Never restore *slot: it now owns the engine's newly created texture.
			return result;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			images_directory = std::filesystem::path(utils::nt::get_host_module().get_folder()) / "consolation" / "images";
			// PC 1.1 DB_LoadGfxImageLoadDefPtr calls the synchronous upload at
			// 0x103D1E2B. Keep native deserialization, asset linking and release.
			upload_hook.create(game::game_offset(0x103ADD20), upload_stub);
		}
	};
}

REGISTER_COMPONENT(image_overrides::component)
