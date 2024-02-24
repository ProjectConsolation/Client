#pragma once

namespace fastfiles
{
	void enum_assets(const game::qos::XAssetType type, const std::function<void(game::qos::XAssetHeader)>& callback, const bool include_override);
}
