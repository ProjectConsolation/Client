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

namespace gameworld
{
	namespace
	{
		game::iw4::gameWorldMp* convert(game::qos::gameWorldMp* gameworld)
		{
			if (!gameworld) return nullptr;

			auto iw4_asset = utils::memory::allocate<game::iw4::gameWorldMp>();
			iw4_asset->name = gameworld->name;

			// glass data generation
			iw4_asset->g_glassData = utils::memory::allocate<game::iw4::G_GlassData>();
			ZeroMemory(iw4_asset->g_glassData, sizeof(game::iw4::G_GlassData));

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
				command::add("dumpgameworld", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumpgameworld <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_gameWORLD_MP, name);
					if (!header.gameWorldMp)
					{
						console::error("dumpgameworld failed on '%s'\n", name);
						return;
					}

					auto converted = gameworld::convert(header.gameWorldMp);
					map_dumper::api->write(game::iw4::ASSET_TYPE_gameWORLD_MP, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_gameWORLD_MP;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.gameWorldMp) };
		}
	};
}

REGISTER_COMPONENT(gameworld::component)
