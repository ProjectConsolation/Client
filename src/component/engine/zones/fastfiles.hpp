#pragma once

namespace fastfiles
{
	void enum_assets(const game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, const bool include_override);
}
