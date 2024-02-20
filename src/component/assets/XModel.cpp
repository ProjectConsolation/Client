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
#include <utils/string.hpp>

namespace xmodel
{
	namespace
	{
		game::iw4::XModel* convert(game::qos::XModel* xmodel)
		{
			if (!xmodel) return nullptr;

			auto iw4_asset = utils::memory::allocate<game::iw4::XModel>();

			iw4_asset->name = xmodel->name;
			iw4_asset->numBones = xmodel->numBones;
			iw4_asset->numRootBones = xmodel->numRootBones;
			iw4_asset->numsurfs = xmodel->numsurfs;
			iw4_asset->lodRampType = xmodel->lodRampType;

			iw4_asset->scale = 1.0f;

			iw4_asset->boneNames = xmodel->boneNames;
			iw4_asset->parentList = xmodel->parentList;
			iw4_asset->quats = xmodel->quats;
			iw4_asset->trans = xmodel->trans;
			iw4_asset->partClassification = xmodel->partClassification;
			iw4_asset->baseMat = xmodel->baseMat;

			iw4_asset->materialHandles = utils::memory::allocate_array<game::iw4::Material*>(xmodel->numsurfs);
			for (size_t i = 0; i < xmodel->numsurfs; i++)
			{
				iw4_asset->materialHandles[i] = assethandler::convert_asset_header(game::qos::ASSET_TYPE_MATERIAL, { xmodel->materialHandles[i] }).material;
			}

			for (int i = 0; i < 4; ++i)
			{
#if EXTEND_CULLING
				iw4_asset->lodInfo[i].dist = xmodel->lodInfo[i].dist * 1.5f; // LOD distance is increased so that the maps look nicer in iw4
#else
				iw4_asset->lodInfo[i].dist = xmodel->lodInfo[i].dist;
#endif
				iw4_asset->lodInfo[i].numsurfs = xmodel->lodInfo[i].numsurfs;
				iw4_asset->lodInfo[i].surfIndex = xmodel->lodInfo[i].surfIndex;

				// 6 vs 4 part bit elements
				std::memcpy(iw4_asset->lodInfo[i].partBits, xmodel->lodInfo[i].partBits, sizeof(xmodel->lodInfo[i].partBits));

				iw4_asset->lodInfo[i].lod = xmodel->lodInfo[i].lod;

#if 0
				// Not correct
				iw4_asset->lodInfo[i].smcBaseIndexPlusOne = xmodel->lodInfo[i].smcIndexPlusOne;
				iw4_asset->lodInfo[i].smcSubIndexMask = xmodel->lodInfo[i].smcAllocBits;
				iw4_asset->lodInfo[i].smcBucket = xmodel->lodInfo[i].unused;
#else
				iw4_asset->lodInfo[i].smcBaseIndexPlusOne = 0;
				iw4_asset->lodInfo[i].smcSubIndexMask = 0;
				iw4_asset->lodInfo[i].smcBucket = 0;
#endif

				if (iw4_asset->lodInfo[i].numsurfs)
				{
					iw4_asset->lodInfo[i].surfs = utils::memory::allocate_array<game::iw4::XSurface>(iw4_asset->lodInfo[i].numsurfs);

					for (unsigned __int16 j = 0; j < iw4_asset->lodInfo[i].numsurfs; ++j)
					{
						game::iw4::XSurface* target = &iw4_asset->lodInfo[i].surfs[j];
						game::qos::XSurface* source = &xmodel->surfs[j + iw4_asset->lodInfo[i].surfIndex];

						target->tileMode = source->tileMode;
						target->deformed = source->deformed;
						target->vertCount = source->vertCount;
						target->triCount = source->triCount;
						target->zoneHandle = 0; //source->zoneHandle;
						target->baseTriIndex = source->baseTriIndex;
						target->baseVertIndex = 0; //source->baseVertIndex;
						target->triIndices = source->triIndices;

						// vertInfo
						memcpy(target->vertInfo.vertCount, source->vertInfo.vertCount, 8);
						target->vertInfo.vertsBlend = source->vertInfo.vertsBlend;

						target->verts0 = source->verts0;

						target->vertListCount = source->vertListCount;
						// vertList
						target->vertList->boneOffset = source->vertList->boneOffset;
						target->vertList->vertCount = source->vertList->vertCount;
						target->vertList->triOffset = source->vertList->triOffset;
						target->vertList->triCount = source->vertList->triCount;
						target->vertList->collisionTree = source->vertList->collisionTree;

						if (i != iw4_asset->collLod)
						{
							for (size_t k = 0; k < target->vertListCount; k++)
							{
								target->vertList[k].collisionTree = nullptr; // Only collod is used
							}
						}

						// 6 vs 4 part bit elements
						std::memcpy(target->partBits, source->partBits, sizeof(source->partBits));
					}

					iw4_asset->lodInfo[i].modelSurfs = utils::memory::allocate<game::iw4::XModelSurfs>();
					iw4_asset->lodInfo[i].modelSurfs->name = utils::memory::duplicate_string(utils::string::va("%s_lod%d", xmodel->name, i & 0xFF));
					iw4_asset->lodInfo[i].modelSurfs->numSurfaces = static_cast<int>(iw4_asset->lodInfo[i].numsurfs);
					iw4_asset->lodInfo[i].modelSurfs->surfaces = iw4_asset->lodInfo[i].surfs;

					// 6 vs 4 part bit elements
					std::memcpy(iw4_asset->lodInfo[i].modelSurfs->partBits, xmodel->lodInfo[i].partBits, sizeof(xmodel->lodInfo[i].partBits));
				}

				iw4_asset->numLods = static_cast<char>(xmodel->numLods);
				iw4_asset->collLod = static_cast<char>(xmodel->collLod);
				iw4_asset->flags = xmodel->flags;

				if (xmodel->collSurfs)
				{
					iw4_asset->collSurfs = utils::memory::allocate_array<game::iw4::XModelCollSurf_s>(xmodel->numCollSurfs);
					for (int i_2 = 0; i_2 < xmodel->numCollSurfs; ++i_2)
					{
						static_assert(sizeof(game::qos::XModelCollSurf_s) == sizeof(game::iw4::XModelCollSurf_s), "Size mismatch");
						std::memcpy(&iw4_asset->collSurfs[i_2], &xmodel->collSurfs[i_2], sizeof(game::iw4::XModelCollSurf_s));
						iw4_asset->collSurfs[i_2].bounds.compute(xmodel->collSurfs[i_2].mins, xmodel->collSurfs[i_2].maxs);
					}
				}

				iw4_asset->numCollSurfs = xmodel->numCollSurfs;
				iw4_asset->contents = xmodel->contents;

				if (xmodel->boneInfo)
				{
					iw4_asset->boneInfo = utils::memory::allocate_array<game::iw4::XBoneInfo>(xmodel->numBones);

					for (char i = 0; i < xmodel->numBones; ++i)
					{
						game::iw4::XBoneInfo* target = &iw4_asset->boneInfo[i];
						game::qos::XBoneInfo* source = &xmodel->boneInfo[i];

						target->radiusSquared = source->radiusSquared;

						target->bounds.compute(source->bounds[0], source->bounds[1]);
						target->bounds.midPoint[0] += source->offset[0];
						target->bounds.midPoint[1] += source->offset[1];
						target->bounds.midPoint[2] += source->offset[2];
					}
				}

				iw4_asset->radius = xmodel->radius;

				iw4_asset->bounds.compute(xmodel->mins, xmodel->maxs);

				iw4_asset->memUsage = xmodel->memUsage;
				iw4_asset->bad = xmodel->bad;
				iw4_asset->physPreset = xmodel->physPreset;

				if (xmodel->physGeoms)
				{
					iw4_asset->physCollmap = utils::memory::allocate<game::iw4::PhysCollmap>();
					iw4_asset->physCollmap->name = utils::memory::duplicate_string(utils::string::va("%s_colmap", xmodel->name));
					iw4_asset->physCollmap->count = xmodel->physGeoms->count;
					iw4_asset->physCollmap->mass = xmodel->physGeoms->mass;
					iw4_asset->physCollmap->bounds = iw4_asset->bounds; // it's fine right?
					iw4_asset->physCollmap->geoms = utils::memory::allocate_array<game::iw4::PhysGeomInfo>(xmodel->physGeoms->count);

					for (unsigned int i = 0; i < xmodel->physGeoms->count; ++i)
					{
						static_assert(sizeof(game::iw4::PhysGeomInfo) == sizeof(game::qos::PhysGeomInfo), "Size mismatch");
						game::iw4::PhysGeomInfo* target = &iw4_asset->physCollmap->geoms[i];
						game::qos::PhysGeomInfo* source = &xmodel->physGeoms->geoms[i];

						std::memcpy(target, source, sizeof(game::iw4::PhysGeomInfo)); // This is ok: IW3 already has "bounds" the way iw4 has, for this struct precisely

						if (source->type >= 4)
						{
							// We're going from
							/*
								PHYS_GEOM_NONE = 0x0,
								PHYS_GEOM_BOX = 0x1,
								PHYS_GEOM_BRUSHMODEL = 0x2,
								PHYS_GEOM_BRUSH = 0x3,
								PHYS_GEOM_CYLINDER = 0x4,
								PHYS_GEOM_CAPSULE = 0x5,
									PHYS_GEOM_COUNT = 0x6,

							to

								PHYS_GEOM_NONE = 0x0,
								PHYS_GEOM_BOX = 0x1,
								PHYS_GEOM_BRUSHMODEL = 0x2,
								PHYS_GEOM_BRUSH = 0x3,
								PHYS_GEOM_COLLMAP = 0x4,
								PHYS_GEOM_CYLINDER = 0x5,
								PHYS_GEOM_CAPSULE = 0x6,
									PHYS_GEOM_GLASS = 0x7,
									PHYS_GEOM_COUNT = 0x8,
							*/

							target->type += 1;

							console::info("Translated physGeomType %i into %i (geom %i from model %s)\n", source->type, target->type, i, xmodel->name);

							if (source->type >= 0x6)
							{
								console::info("Unexpected geom type %i (geom %i from model %s)\n", source->type, i, xmodel->name);
							}
						}

						if (source->brush)
						{
							target->brush = utils::memory::allocate<game::iw4::BrushWrapper>();
							target->brush->bounds.compute(source->brush->mins, source->brush->maxs);

							target->brush->brush.numsides = static_cast<unsigned short>(source->brush->numsides);
							target->brush->brush.baseAdjacentSide = source->brush->baseAdjacentSide;
							target->brush->brush.glassPieceIndex = 0; // IW3's content is not mapped, but I doubt it belongs to this

							std::memcpy(target->brush->brush.axialMaterialNum, source->brush->axialMaterialNum, sizeof(source->brush->axialMaterialNum));
							std::memcpy(target->brush->brush.edgeCount, source->brush->edgeCount, sizeof(source->brush->edgeCount));

							for (int k = 0; k < 2; ++k)
							{
								for (int j = 0; j < 3; ++j)
								{
									target->brush->brush.firstAdjacentSideOffsets[k][j] = static_cast<char>(source->brush->firstAdjacentSideOffsets[k][j]);
								}
							}

							target->brush->planes = source->brush->planes;
							target->brush->totalEdgeCount = source->brush->totalEdgeCount;

							target->brush->brush.sides = utils::memory::allocate_array<game::iw4::cbrushside_t>(target->brush->brush.numsides);

							for (unsigned short j = 0; j < target->brush->brush.numsides; ++j)
							{
								game::iw4::cbrushside_t* targetSide = &target->brush->brush.sides[j];
								game::qos::cbrushside_t* sourceSide = &source->brush->sides[j];

								targetSide->plane = sourceSide->plane;
								targetSide->materialNum = static_cast<unsigned short>(sourceSide->materialNum);
								targetSide->firstAdjacentSideOffset = static_cast<char>(sourceSide->firstAdjacentSideOffset);
								targetSide->edgeCount = sourceSide->edgeCount;
							}
						}
					}

					// Physical collision maps don't work!
					// Porting these on iw4 maps result in models with fucked up physics bouncing everywhere and making noise
					// and in some cases results in freezes if too many of them move over (fruits in carentan, papers on backlot)
					// Killing the physCollmap during porting solves this issue with no apparent drawback, surprisingly
					// The objects (car doors, fruits, ...) still work as expected when this is null. So let's keep it that way until we know what's going on!
					// The error may lie in the above codeblock, or in zonebuilder's reading of physcollmaps.
					iw4_asset->physCollmap = nullptr;
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
				command::add("dumpxmodel", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumpxmodel <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_XMODEL, name);
					if (!header.xmodel)
					{
						console::error("dumpxmodel failed on '%s'\n", name);
						return;
					}

					auto converted = xmodel::convert(header.xmodel);
					map_dumper::api->write(game::iw4::ASSET_TYPE_XMODEL, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_XMODEL;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.xmodel) };
		}
	};
}

REGISTER_COMPONENT(xmodel::component)
