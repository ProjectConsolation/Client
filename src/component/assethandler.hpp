#pragma once

namespace assethandler
{
	extern std::unordered_map<game::qos::XAssetType, game::iw4::XAssetType> type_conversion_table;

	game::iw4::XAssetHeader convert_asset_header(game::qos::XAssetType type, game::qos::XAssetHeader header);
	void dump_asset(game::qos::XAssetType type, game::qos::XAssetHeader asset);
}
