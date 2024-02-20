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

namespace rawfile
{
	namespace
	{
		game::iw4::RawFile* convert(game::qos::RawFile* rawfile)
		{
			if (!rawfile || !rawfile->buffer) return nullptr;

			auto iw4_asset = utils::memory::allocate<game::iw4::RawFile>();
			iw4_asset->name = rawfile->name;
			iw4_asset->compressedLen = 0;
			iw4_asset->len = rawfile->len;
			iw4_asset->buffer = rawfile->buffer;
			return iw4_asset;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::once([&]()
			{
				command::add("dumprawfile", [](const command::params& params)
				{
					if (params.size() < 2)
					{
						console::info("USAGE: dumprawfile <name>\n");
						return;
					}

					const auto name = params[1];

					auto header = game::DB_FindXAssetHeader(game::qos::ASSET_TYPE_RAWFILE, name);
					if (!header.rawfile)
					{
						console::error("dumprawfile failed on '%s'\n", name);
						return;
					}

					auto converted = rawfile::convert(header.rawfile);
					map_dumper::api->write(game::iw4::ASSET_TYPE_RAWFILE, converted);

					console::info("dumped '%s' for IW4\n", name);
				});
			}, scheduler::main);
		}

		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_RAWFILE;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.rawfile) };
		}
	};
}

REGISTER_COMPONENT(rawfile::component)
