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

namespace gfxworld
{
	namespace
	{
		game::iw4::GfxWorld* convert(game::qos::GfxWorld* world)
		{
			if (!world) return nullptr;

			game::iw4::GfxSky* sky = utils::memory::allocate<game::iw4::GfxSky>();

			auto iw4_asset = utils::memory::allocate<game::iw4::GfxWorld>();

			iw4_asset->name = world->name;
			iw4_asset->baseName = world->baseName;
			iw4_asset->planeCount = world->planeCount;

			// planes
			iw4_asset->dpvsPlanes.cellCount = world->planes->cellCount;
			memcpy(iw4_asset->dpvsPlanes.planes, world->planes->planes, 4);
			memcpy(iw4_asset->dpvsPlanes.nodes, world->planes->nodes, 4);
			memcpy(iw4_asset->dpvsPlanes.sceneEntCellBits, world->planes->sceneEntCellBits, 4);

			iw4_asset->nodeCount = world->nodeCount;
			// TODO: nodes?

			//iw4_asset->indexCount = world->indexCount;

			iw4_asset->surfaceCount = world->surfaceCount;
			if (world->surfaces)
			{
				iw4_asset->dpvs.surfaces = utils::memory::allocate_array<game::iw4::GfxSurface>(world->surfaceCount);
				iw4_asset->dpvs.surfacesBounds = utils::memory::allocate_array < game::iw4::GfxSurfaceBounds(world->surfaceCount);

				for (int i_ = 0; i_ < world->surfaceCount; i_++)
				{
					iw4_asset->dpvs.surfaces[i_].tris = world->surfaces[i_].tris;
					iw4_asset->dpvs.surfaces[i_].material = assethandler::convert_asset_header(game::qos::ASSET_TYPE_MATERIAL, { world->surfaces[i_].material }).material;
					
					iw4_asset->dpvs.surfaces[i_].lightmapIndex = world->surfaces[i_].lightmapIndex;
					iw4_asset->dpvs.surfaces[i_].reflectionProbeIndex = world->surfaces[i_].reflectionProbeIndex;
					iw4_asset->dpvs.surfaces[i_].primaryLightIndex = world->surfaces[i_].primaryLightIndex;
					iw4_asset->dpvs.surfaces[i_].flags = world->surfaces[i_].flags;

					iw4_asset->dpvs.surfacesBounds[i_].bounds.compute(world->surfaces[i_].bounds[0], world->surfaces[i_].bounds[1]);
				}
			}

			iw4_asset->skyCount = 1;
			iw4_asset->skies = sky;

			if (world->skyImage)
			{
				sky->skyImage = assethandler::convert_asset_header(game::qos::ASSET_TYPE_IMAGE, { world->skyImage }).image;
			}

			sky->skySamplerState = world->skySamplerState/* & 0xFF*/;
			sky->skyStartSurfs = world->skyStartSurfs;
			sky->skySurfCount = world->skySurfCount;

			iw4_asset->lastSunPrimaryLightIndex = world->sunPrimaryLightIndex;
			iw4_asset->primaryLightCount = world->primaryLightCount;

			// dpvsPlanes
			auto dpvsPlanes = *world->planes;
			iw4_asset->dpvsPlanes.cellCount = dpvsPlanes.cellCount;
			memcpy(iw4_asset->dpvsPlanes.planes, dpvsPlanes.planes, 4);
			memcpy(iw4_asset->dpvsPlanes.nodes, dpvsPlanes.nodes, 4);
			memcpy(iw4_asset->dpvsPlanes.sceneEntCellBits, dpvsPlanes.sceneEntCellBits, 4);

			iw4_asset->dpvs.surfaceMaterials = utils::memory::allocate_array<game::iw4::GfxDrawSurf>(world->surfaceCount);
			for (auto i = 0; i < world->surfaceCount; i++)
			{
				iw4_asset->dpvs.surfaceMaterials[i].fields.objectId = world->surfaceMaterials[i].fields.objectId;
				iw4_asset->dpvs.surfaceMaterials[i].fields.reflectionProbeIndex = world->surfaceMaterials[i].fields.reflectionProbeIndex;
				iw4_asset->dpvs.surfaceMaterials[i].fields.customIndex = world->surfaceMaterials[i].fields.customIndex;
				iw4_asset->dpvs.surfaceMaterials[i].fields.materialSortedIndex = world->surfaceMaterials[i].fields.materialSortedIndex;
				iw4_asset->dpvs.surfaceMaterials[i].fields.prepass = world->surfaceMaterials[i].fields.prepass;
				iw4_asset->dpvs.surfaceMaterials[i].fields.sceneLightIndex = world->surfaceMaterials[i].fields.primaryLightIndex;
				iw4_asset->dpvs.surfaceMaterials[i].fields.surfType = world->surfaceMaterials[i].fields.surfType;
				iw4_asset->dpvs.surfaceMaterials[i].fields.primarySortKey = world->surfaceMaterials[i].fields.primarySortKey;
				iw4_asset->dpvs.surfaceMaterials[i].fields.unused = world->surfaceMaterials[i].fields.unused;
			}

			iw4_asset->dpvs.surfaceCastsSunShadow = world->surfaceCastsSunShadow;
			iw4_asset->dpvs.usageCount = 0; // ?? (world->usageCount)

			// dpvsDyn
			//iw4_asset->dpvsDyn.dynEntClientCount = world->dynEntClientCount;
			//iw4_asset->dpvsDyn.dynEntClientWordCount = world->dynEntClientWordCount;

			// iw4_credits has 0x3
			// Probably do not add Game::IW4::FogTypes::FOG_DFOG here, cod4 doesn't support it !
			iw4_asset->fogTypesAllowed = game::iw4::FOG_NORMAL;

			// AABBTree data is stored as part of the cells.
			// however, in IW4 it's not, so we have to extract the data
			if (world->cells)
			{
				iw4_asset->aabbTreeCounts = utils::memory::allocate_array<game::iw4::GfxCellTreeCount>(world->planes->cellCount);
				iw4_asset->aabbTrees = utils::memory::allocate_array<game::iw4::GfxCellTree>(world->planes->cellCount);
				iw4_asset->cells = utils::memory::allocate_array<game::iw4::GfxCell>(world->planes->cellCount);

				for (int i_ = 0; i_ < world->planes->cellCount; ++i_)
				{
					iw4_asset->aabbTreeCounts[i_].aabbTreeCount = world->cells[i_].aabbTree;
				}
			}

			return { iw4_asset };
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::once([&]()
			{
				command::add("dumpgfxworld", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumpgfxworld <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_GFXWORLD, name);
					if (!header.comWorld)
					{
						console::error("dumpgfxworld failed on '%s'\n", name);
						return;
					}

					auto converted = gfxworld::convert(header.gfxWorld);
					map_dumper::api->write(game::iw4::ASSET_TYPE_GFXWORLD, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_GFXWORLD;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.gfxWorld) };
		}
	};
}

REGISTER_COMPONENT(gfxworld::component)
