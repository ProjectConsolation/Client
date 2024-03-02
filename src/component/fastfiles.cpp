#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "fastfiles.hpp"
#include "command.hpp"
#include "console.hpp"

#include <utils/hook.hpp>
#include <utils/memory.hpp>
#include <utils/io.hpp>

namespace fastfiles
{
	void enum_assets(const game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, const bool include_override)
	{
		game::DB_EnumXAssets_FastFile(type, static_cast<void(*)(game::XAssetHeader, void*)>([](game::XAssetHeader header, void* data)
			{
				const auto& cb = *static_cast<const std::function<void(game::XAssetHeader)>*>(data);
				cb(header);
			}), &callback, include_override);
	}

	class component final : public component_interface
	{
		/*public:
			void post_unpack() override
			{
				//Placeholder for now, so it builds
			}*/
	};
}

REGISTER_COMPONENT(fastfiles::component)
