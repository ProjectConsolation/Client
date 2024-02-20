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

namespace comworld
{
	namespace
	{
		game::iw4::ComWorld* convert(game::qos::ComWorld* comworld)
		{
			if (!comworld) return nullptr;

			auto iw4_asset = utils::memory::allocate<game::iw4::ComWorld>();
			iw4_asset->name = comworld->name;
			iw4_asset->isInUse = comworld->isInUse;
			iw4_asset->primaryLightCount = comworld->primaryLightCount;

			iw4_asset->primaryLights = utils::memory::allocate_array<game::iw4::ComPrimaryLight>(iw4_asset->primaryLightCount);
			for (size_t i = 0; i < iw4_asset->primaryLightCount; i++)
			{
				const auto qos_light = &comworld->primaryLights[i];
				auto iw4_light = &iw4_asset->primaryLights[i];

				if (qos_light->defName)
				{
					iw4_light->defName = utils::memory::duplicate_string(qos_light->defName);
				}

				// gamma correction (needed for QoS?)
				for (size_t channel = 0; channel < 3; channel++)
				{
					// IW4 uses 2.4F for gamma, not 2.2
					iw4_light->color[channel] = std::pow(qos_light->color[channel], 1.f / 2.4f);
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
				command::add("dumpcomworld", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumpcomworld <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_COMWORLD, name);
					if (!header.comWorld)
					{
						console::error("dumpcomworld failed on '%s'\n", name);
						return;
					}

					auto converted = comworld::convert(header.comWorld);
					map_dumper::api->write(game::iw4::ASSET_TYPE_COMWORLD, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_COMWORLD;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.comWorld) };
		}
	};
}

REGISTER_COMPONENT(comworld::component)
