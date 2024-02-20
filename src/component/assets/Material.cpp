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

namespace material
{
	namespace
	{
		game::iw4::Material* convert(game::qos::Material* material)
		{
			if (!material) return nullptr;

			auto iw4_asset = utils::memory::allocate<game::iw4::Material>();
			auto iw4_asset_newSurf = utils::memory::allocate<game::iw4::GfxDrawSurf>();

			iw4_asset->name = material->info.name;
			iw4_asset->gameFlags.packed = material->info.gameFlags.packed;
			iw4_asset->gameFlags.fields.unk7 = material->info.gameFlags.fields.unkNeededForSModelDisplay;
			iw4_asset->gameFlags.fields.unkCastShadowMaybe = material->info.gameFlags.fields.MTL_GAMEFLAG_CASTS_SHADOW;
			iw4_asset->sortKey = material->info.sortKey;

			iw4_asset->textureAtlasRowCount = material->info.textureAtlasRowCount;
			iw4_asset->textureAtlasColumnCount = material->info.textureAtlasColumnCount;

			iw4_asset_newSurf->fields.objectId = material->info.drawSurf.fields.objectId;
			iw4_asset_newSurf->fields.reflectionProbeIndex = material->info.drawSurf.fields.reflectionProbeIndex;
			iw4_asset_newSurf->fields.customIndex = material->info.drawSurf.fields.customIndex;
			iw4_asset_newSurf->fields.materialSortedIndex = material->info.drawSurf.fields.materialSortedIndex;
			iw4_asset_newSurf->fields.prepass = material->info.drawSurf.fields.prepass;
			iw4_asset_newSurf->fields.sceneLightIndex = material->info.drawSurf.fields.primaryLightIndex;
			iw4_asset_newSurf->fields.surfType = material->info.drawSurf.fields.surfType;
			iw4_asset_newSurf->fields.primarySortKey = material->info.drawSurf.fields.primarySortKey;
			iw4_asset->drawSurf.packed = iw4_asset_newSurf->packed;

			iw4_asset->surfaceTypeBits = material->info.surfaceTypeBits;
			iw4_asset->hashIndex = material->info.hashIndex;

			// Set them all to -1 so they're not used if they dont exist in iw3
			std::memset(iw4_asset->stateBitsEntry, 0xFF, sizeof(iw4_asset->stateBitsEntry));

			// copy techniques to correct spots
			/*
			for (size_t i = 0; i < game::iw4::TECHNIQUE_COUNT; i++)
			{
				game::iw4::MaterialTechniqueType technique = static_cast<game::iw4::MaterialTechniqueType>(i);
				if (IMaterialTechniqueSet::techniqueTypeTableFromIW4.contains(technique))
				{
					iw4_asset->stateBitsEntry[technique] = material->stateBitsEntry[IMaterialTechniqueSet::techniqueTypeTableFromIW4.at(technique)];
				}
				else
				{
					// Not necessary
					iw4_asset->stateBitsEntry[technique] = 0xFF;
				}
			}
			*/

			iw4_asset->textureCount = material->textureCount;
			iw4_asset->constantCount = material->constantCount;
			iw4_asset->stateBitsCount = material->stateBitsCount;
			iw4_asset->stateFlags = static_cast<game::iw4::StateFlags>(material->stateFlags); // Correspondance is identical

			if (iw4_asset->sortKey == 0)
			{
				iw4_asset->stateFlags = static_cast<game::iw4::StateFlags>(iw4_asset->stateFlags | game::iw4::STATE_FLAG_AMBIENT);
				console::info("Added STATE_FLAG_AMBIENT on %s (sortkey is opaque-ambient)\n", iw4_asset->name);
			}

			// Should we give ambient stateflag to everybody by default?
			iw4_asset->cameraRegion = material->cameraRegion;

			if (iw4_asset->cameraRegion == 0x3) 
			{
				// 0x3 is NONE in iw3, but DEPTH_HACK in iw4
				// In iw4 NONE is 0x4
				console::info("Swapped material %s camera region from 0x3 to 0x4 (NONE)\n", iw4_asset->name);
				iw4_asset->cameraRegion = 0x4;
			}

			//iw4_asset->techniqueSet = AssetHandler::Convert(Game::IW3::XAssetType::ASSET_TYPE_TECHNIQUE_SET, { material->techniqueSet }).techniqueSet;

			iw4_asset->textureTable = utils::memory::allocate_array<game::iw4::MaterialTextureDef>(iw4_asset->textureCount);
			for (char i = 0; i < iw4_asset->textureCount; i++)
			{
				auto qosDef = &material->textureTable[i];
				auto iw4Def = &iw4_asset->textureTable[i];

				static_assert(sizeof game::iw4::MaterialTextureDef == sizeof game::qos::MaterialTextureDef);
				std::memcpy(iw4Def, qosDef, sizeof game::iw4::MaterialTextureDef);


				if (iw4Def->semantic == 0xB) // TS_Water
				{
					console::error("Doesn't support water for now");
				}
				else
				{
					auto imageAsset = assethandler::convert_asset_header(game::qos::ASSET_TYPE_IMAGE, { qosDef->image }).image;
					iw4Def->u.image = imageAsset;
				}
			}

			iw4_asset->constantTable = nullptr;
			if (material->constantTable)
			{
				iw4_asset->constantTable = utils::memory::allocate_array<game::qos::MaterialConstantDef>(material->constantCount);

				for (char i = 0; i < material->constantCount; ++i)
				{
					game::qos::MaterialConstantDef* constantDef = &material->constantTable[i];
					game::qos::MaterialConstantDef* targetDef = &iw4_asset->constantTable[i];

					std::memcpy(targetDef, constantDef, sizeof(game::qos::MaterialConstantDef));

					if (targetDef->name == "envMapParms"s)
					{
						// These use the speculars to add some rimlight effects to models
						// But since speculars are regenerated we end up with cod6 speculars with cod4 materials
						// and cod6 speculars are a bit too bright for 

						targetDef->literal[0] *= 0.0875f; // envMapMin
						targetDef->literal[1] *= 0.2f;  // envMapMax
						targetDef->literal[2] *= 1.4f;    // engMapExponent
						targetDef->literal[3] *= 2.2f;    // envMapIntensity
					}

				}
			}

			iw4_asset->stateBitTable = material->stateBitTable;

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
				command::add("dumpmaterial", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumpmaterial <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_MATERIAL, name);
					if (!header.material)
					{
						console::error("dumpmaterial failed on '%s'\n", name);
						return;
					}

					auto converted = material::convert(header.material);
					map_dumper::api->write(game::iw4::ASSET_TYPE_MATERIAL, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_MATERIAL;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.material) };
		}
	};
}

REGISTER_COMPONENT(material::component)
