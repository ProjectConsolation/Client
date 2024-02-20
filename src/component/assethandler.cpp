#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/map_dumper.hpp"

#include "game/game.hpp"
#include "game/structs.IW4.hpp"

#include <utils/memory.hpp>

#define QOS_TYPE_TO_IW4(type) { game::qos::type, game::iw4::type },

namespace assethandler
{
	struct processed_asset
	{
		void* original_ptr;
		game::iw4::XAssetHeader converted_asset;
		game::qos::XAssetType qos_type;
	};

	std::vector<processed_asset> converted_assets;

	std::unordered_map<game::qos::XAssetType, game::iw4::XAssetType> type_conversion_table = {
		//QOS_TYPE_TO_IW4(ASSET_TYPE_XMODELPIECES)		// not on IW4?
		QOS_TYPE_TO_IW4(ASSET_TYPE_PHYSPRESET)
		//QOS_TYPE_TO_IW4(ASSET_TYPE_PHYSCONSTRAINTS)	// assets are from T4 and dont exist on IW3/IW4
		//QOS_TYPE_TO_IW4(ASSET_TYPE_DESTRUCTIBLE_DEF)	// ^
		QOS_TYPE_TO_IW4(ASSET_TYPE_XANIMPARTS)
		QOS_TYPE_TO_IW4(ASSET_TYPE_XMODEL)
		QOS_TYPE_TO_IW4(ASSET_TYPE_MATERIAL)
		QOS_TYPE_TO_IW4(ASSET_TYPE_TECHNIQUE_SET)
		QOS_TYPE_TO_IW4(ASSET_TYPE_IMAGE)
		QOS_TYPE_TO_IW4(ASSET_TYPE_SOUND)
		QOS_TYPE_TO_IW4(ASSET_TYPE_SOUND_CURVE)
		//QOS_TYPE_TO_IW4(ASSET_TYPE_LOADED_SOUND)		// no loaded sounds in QoS?
		QOS_TYPE_TO_IW4(ASSET_TYPE_CLIPMAP_SP)
		QOS_TYPE_TO_IW4(ASSET_TYPE_CLIPMAP_MP)
		QOS_TYPE_TO_IW4(ASSET_TYPE_COMWORLD)
		QOS_TYPE_TO_IW4(ASSET_TYPE_gameWORLD_SP)
		QOS_TYPE_TO_IW4(ASSET_TYPE_gameWORLD_MP)
		QOS_TYPE_TO_IW4(ASSET_TYPE_MAP_ENTS)
		QOS_TYPE_TO_IW4(ASSET_TYPE_GFXWORLD)
		QOS_TYPE_TO_IW4(ASSET_TYPE_LIGHT_DEF)
		QOS_TYPE_TO_IW4(ASSET_TYPE_UI_MAP)
		QOS_TYPE_TO_IW4(ASSET_TYPE_FONT)
		QOS_TYPE_TO_IW4(ASSET_TYPE_MENULIST)
		QOS_TYPE_TO_IW4(ASSET_TYPE_MENU)
		QOS_TYPE_TO_IW4(ASSET_TYPE_LOCALIZE_ENTRY)
		QOS_TYPE_TO_IW4(ASSET_TYPE_WEAPON)
		QOS_TYPE_TO_IW4(ASSET_TYPE_SNDDRIVER_GLOBALS)
		QOS_TYPE_TO_IW4(ASSET_TYPE_FX)
		QOS_TYPE_TO_IW4(ASSET_TYPE_IMPACT_FX)
		QOS_TYPE_TO_IW4(ASSET_TYPE_AITYPE)
		QOS_TYPE_TO_IW4(ASSET_TYPE_MPTYPE)
		QOS_TYPE_TO_IW4(ASSET_TYPE_CHARACTER)
		QOS_TYPE_TO_IW4(ASSET_TYPE_XMODELALIAS)
		QOS_TYPE_TO_IW4(ASSET_TYPE_RAWFILE)
		QOS_TYPE_TO_IW4(ASSET_TYPE_STRINGTABLE)

		// these assets only exist in QoS
		//QOS_TYPE_TO_IW4(ASSET_TYPE_XML_TREE)
		//QOS_TYPE_TO_IW4(ASSET_TYPE_SCENE_ANIM_RESOURCE)
		//QOS_TYPE_TO_IW4(ASSET_TYPE_CUTSCENE_RESOURCE)
		//QOS_TYPE_TO_IW4(ASSET_TYPE_CUSTOM_CAMERA_LIST)

		QOS_TYPE_TO_IW4(ASSET_TYPE_COUNT)
		QOS_TYPE_TO_IW4(ASSET_TYPE_STRING)
		QOS_TYPE_TO_IW4(ASSET_TYPE_ASSETLIST)
	};

	utils::memory::allocator local_allocator{};

	game::iw4::XAssetHeader convert_asset_header(game::qos::XAssetType type, game::qos::XAssetHeader header)
	{
		const auto& existing = std::find_if(converted_assets.begin(), converted_assets.end(), [&](processed_asset& asset)
		{
			return (asset.qos_type == type && asset.original_ptr == header.data);
		});

		if (existing != converted_assets.end())
		{
			return { existing._Ptr->converted_asset.data };
		}

		// TODO: review this, this is probably veryyy slow to do lol
		for (const auto& component_ : component_loader::get_components())
		{
			if (component_->get_type() != game::qos::ASSET_TYPE_INVALID &&
				component_->get_type() == type)
			{
				const auto& result = component_->convert_asset(header);
				converted_assets.push_back({ header.data, result, type });
				return result;
			}
		}

		return { nullptr };
	}

	// TODO:
	void dump_asset(game::qos::XAssetType type, game::qos::XAssetHeader asset)
	{
		const auto converted = convert_asset_header(type, asset);
		if (converted.data)
		{
			map_dumper::api->write(type_conversion_table[type], converted.data);
		}
	}

	class component final : public component_interface
	{
	public:
		void pre_destroy() override
		{
			converted_assets.clear();
		}
	};
}

REGISTER_COMPONENT(assethandler::component)
