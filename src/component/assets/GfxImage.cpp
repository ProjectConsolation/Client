#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/assethandler.hpp"
#include "component/command.hpp"
#include "component/console.hpp"
#include "component/map_dumper.hpp"
#include "component/scheduler.hpp"

#include "game/game.hpp"
#include "game/structs.IW4.hpp"

#include <utils/memory.hpp>
#include <utils/hook.hpp>

#define IW4X_IMG_VERSION "0"

static_assert(sizeof game::iw4::GfxImageFileHeader::tag == sizeof game::qos::GfxImageFileHeader::tag, "tag size mismatch!");
static_assert(sizeof game::iw4::GfxImageFileHeader::dimensions == sizeof game::qos::GfxImageFileHeader::dimensions, "dimensions size mismatch!");
static_assert(sizeof game::iw4::GfxImageFileHeader::fileSizeForPicmip == sizeof game::qos::GfxImageFileHeader::fileSizeForPicmip, "fileSizeForPicmip size mismatch!");

namespace gfximage
{
	namespace
	{
		static const std::unordered_set<std::string> images_to_rename =
		{
			"xpec_electrical_transformer_col", // Used between IW3 and IW4 but very different texture
			"xpec_electrical_transformer_nml",
			"refrigerator2_col", // Blue fridge in IW4, red in IW3
			"refrigerator2_nml",
			"refrigerator_col", // Different fridge texture
			"refrigerator_nml",
		};

		//static std::unordered_map<std::string, std::string> renamed_images{};
		//static std::unordered_map<std::string, std::string> renamed_images_reverse{};

		//const std::string image_suffix = "_3x";

		int translate_flags(int iw3Flags)
		{
			int iw4Flags = 0;

			const auto translate_flags_func = [&](game::qos::file_image_flags_t sourceFlag, game::iw4::file_image_flags_t targetFlag)
				{
					if (iw3Flags & sourceFlag)
					{
						iw4Flags |= targetFlag;
					}
				};

			translate_flags_func(game::qos::IMG_FLAG_NOPICMIP, game::iw4::IMG_FLAG_NOPICMIP);
			translate_flags_func(game::qos::IMG_FLAG_NOMIPMAPS, game::iw4::IMG_FLAG_NOMIPMAPS);
			translate_flags_func(game::qos::IMG_FLAG_CUBEMAP, game::iw4::IMG_FLAG_MAPTYPE_CUBE);
			translate_flags_func(game::qos::IMG_FLAG_VOLMAP, game::iw4::IMG_FLAG_MAPTYPE_3D);
			translate_flags_func(game::qos::IMG_FLAG_STREAMING, game::iw4::IMG_FLAG_STREAMING);
			translate_flags_func(game::qos::IMG_FLAG_LEGACY_NORMALS, game::iw4::IMG_FLAG_LEGACY_NORMALS);
			translate_flags_func(game::qos::IMG_FLAG_CLAMP_U, game::iw4::IMG_FLAG_CLAMP_U);
			translate_flags_func(game::qos::IMG_FLAG_CLAMP_V, game::iw4::IMG_FLAG_CLAMP_V);

			// Not sure about these
			//translate_flags_func(game::qos::IMG_FLAG_DYNAMIC,        game::iw4::IMG_FLAG_DYNAMIC);
			//translate_flags_func(game::qos::IMG_FLAG_RENDER_TARGET,  game::iw4::IMG_FLAG_RENDER_TARGET);
			//translate_flags_func(game::qos::IMG_FLAG_SYSTEMMEM,      game::iw4::IMG_FLAG_SYSTEMMEM);

			//iw4Flags |= game::iw4::IMG_FLAG_ALPHA_WEIGHTED_COLORS;
			//iw4Flags |= game::iw4::IMG_FLAG_GAMMA_SRGB;

			return iw4Flags;
		}

		game::iw4::GfxImageLoadDef* convert_load_def(game::qos::GfxImageLoadDef* load_def)
		{
			auto iw4_def = reinterpret_cast<game::iw4::GfxImageLoadDef*>(utils::memory::allocate(sizeof(game::iw4::GfxImageLoadDef) + load_def->resourceSize - 4));

			std::memcpy(iw4_def->data, load_def->data, load_def->resourceSize);
			iw4_def->flags = translate_flags(load_def->flags);
			iw4_def->format = load_def->format;
			iw4_def->levelCount = load_def->levelCount;
			iw4_def->resourceSize = load_def->resourceSize;

			return iw4_def;
		}

		game::iw4::GfxImage* convert(game::qos::GfxImage* image)
		{
			if (!image) return nullptr;

			std::string name = image->name;
			if (name[0] == '*') name.erase(name.begin());

			auto iw4_asset = utils::memory::allocate<game::iw4::GfxImage>();
			
			iw4_asset->texture.loadDef = convert_load_def(image->texture.loadDef);

			iw4_asset->mapType = static_cast<unsigned char>(image->mapType);
			iw4_asset->semantic = static_cast<unsigned char>(image->semantic);
			iw4_asset->category = 0;//image->category;
			iw4_asset->picmip = image->picmip;
			iw4_asset->noPicmip = image->noPicmip;
			iw4_asset->track = image->track;
			iw4_asset->cardMemory = image->cardMemory;
			iw4_asset->width = image->width;
			iw4_asset->height = image->height;
			iw4_asset->depth = image->depth;
			iw4_asset->delayLoadPixels = image->delayLoadPixels;
			iw4_asset->name = image->name;

			return { iw4_asset };
		}

		utils::hook::detour store_texture_hook;
		void store_texture_stub()
		{
			game::qos::GfxImageLoadDef** loadDef = *reinterpret_cast<game::qos::GfxImageLoadDef***>(game::game_offset(0x10AB8F58));
			game::qos::GfxImage* image = *reinterpret_cast<game::qos::GfxImage**>(game::game_offset(0x10AB8E00));

			size_t size = 16 + (*loadDef)->resourceSize;
			void* data = utils::memory::get_allocator()->allocate(size);
			std::memcpy(data, *loadDef, size);

			image->texture.loadDef = reinterpret_cast<game::qos::GfxImageLoadDef*>(data);
		}

		utils::hook::detour release_texture_hook;
		void release_texture_stub(game::qos::XAssetHeader header)
		{
			if (header.image && header.image->texture.loadDef)
			{
				utils::memory::get_allocator()->free(header.image->texture.loadDef);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			//store_texture_hook.create(game::game_offset(0x103ADD20), store_texture_stub);
			//release_texture_hook.create(game::game_offset(0x103ADD20), release_texture_stub);

			scheduler::once([&]()
			{
				command::add("dumpgfximage", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumpgfximage <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_IMAGE, name);
					if (!header.image)
					{
						console::error("dumpgfximage failed on '%s'\n", name);
						return;
					}

					auto converted = gfximage::convert(header.image);
					map_dumper::api->write(game::iw4::ASSET_TYPE_IMAGE, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_IMAGE;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.image) };
		}
	};
}

REGISTER_COMPONENT(gfximage::component)
