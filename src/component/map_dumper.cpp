#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/assethandler.hpp"
#include "component/command.hpp"
#include "component/console.hpp"
#include "component/map_dumper.hpp"
#include "component/scheduler.hpp"

#include "game/game.hpp"
#include "game/structs.IW4.hpp"

#include <utils/io.hpp>
#include <utils/string.hpp>

namespace map_dumper
{
	iw4of::api* api = nullptr;

	namespace
	{
		std::string api_read_file(const std::string& name)
		{
			/*
			if (name.ends_with(".iwi"))
			{
				return IGfxImage::ConvertIWIOnTheFly(filename);
			}
			*/

			std::string data;
			if (utils::io::read_file(name, &data))
			{
				return data;
			}

			return "";
		}

		iw4of::params_t get_params()
		{
			auto params = iw4of::params_t();
			params.write_only_once = true;
			params.fs_read_file = api_read_file;
			params.get_from_string_table = [](unsigned int index)
			{
				const char* result = 0;
				if (index)
				{
					result = &game::scrMemTreePub->mt_buffer[8 * index + 4];
				}
				return result;
			};

			params.find_other_asset = [](int type, const std::string& name)
			{
				for (const auto& kv : assethandler::type_conversion_table)
				{
					if (kv.second == type)
					{
						auto qos_type = kv.first;
						std::string name_to_find = name;

						/*
						if (qos_type == game::qos::ASSET_TYPE_WEAPON)
						{
							// Fix weapon name
							name_to_find = name.substr(4); // Remove iw3_ prefix while seeking
						}
						*/

						auto header = game::DB_FindXAssetHeader(qos_type, name_to_find.data());
						if (header.data && !game::DB_IsXAssetDefault(qos_type, name_to_find.data()))
						{
							return assethandler::convert_asset_header(qos_type, header).data;
						}

						return static_cast<void*>(nullptr);
					}
				}

				return static_cast<void*>(nullptr);
			};

			params.print = [](int level, const std::string& message)
			{
				if (level)
				{
					console::error("%s", message.data());
					assert(false);
				}
				else
				{
					console::debug("%s", message.data());
				}
			};

			params.work_directory = "qosxport_out/default";

			return params;
		}

		void dump_map(std::string name)
		{
			bool is_singleplayer = !name.starts_with("mp_");
			std::string bsp_name = utils::string::va("maps/%s%s.d3dbsp", is_singleplayer ? "" : "mp/", name.data());

			console::info("loading map '%s'...\n", name.data());
			command::execute(utils::string::va("%s %s", is_singleplayer ? "loadzone" : "map", name.data()));
			command::execute(utils::string::va("loadzone %s_load", name.data()));

			// TODO: export sounds (Louve seems to have some big function for this, i'll do it later lol)
			console::info("exporting all sounds...\n");

			console::info("exporting ComWorld...\n");
			command::execute(utils::string::va("dumpcomworld %s", bsp_name.data()));

			console::info("exporting GameWorld...\n");
			command::execute(utils::string::va("dumpgameworld %s", bsp_name.data()));

			// this is redundant with clipmap but allows exporting more models
			console::info("exporting Entities...\n");
			command::execute(utils::string::va("dumpmapents %s", bsp_name.data()));

			// TODO:
			/*
			console::info("exporting GfxWorld...\n");
			command::execute(utils::string::va("dumpgfxworld %s", bsp_name.data()));
			*/
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			api = new iw4of::api(get_params());

			command::add("dumpmap", [](const command::params& params)
			{
#ifndef DEBUG
				console::error("dumpmap is not supported yet!\n");
				return;
#else
				if (params.size() < 2)
				{
					console::info("USAGE: dumpmap <name>\n");
					return;
				}

				console::warn("dumpmap is not fully supported yet!\n");

				std::string name = params[1];

				api->set_work_path("qosxport_out/dump");

				dump_map(name);
				console::info("map '%s' successfully exported.\n", name.data());
			});
#endif
		}
	};
}

REGISTER_COMPONENT(map_dumper::component)
