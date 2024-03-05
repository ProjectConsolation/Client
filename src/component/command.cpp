#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"

#include <utils/memory.hpp>
#include <utils/string.hpp>
#include <utils/io.hpp>
#include "fastfiles.hpp"
#include <game/dvars.hpp>

namespace command
{
	namespace
	{
		static utils::memory::allocator cmd_allocator;

		std::unordered_map<std::string, std::function<void(params&)>> handlers;

		void main_handler()
		{
			params params = {};

			const auto command = utils::string::to_lower(params[0]);
			if (handlers.find(command) != handlers.end())
			{
				handlers[command](params);
			}
		}
	}

	params::params()
		: nesting_(*game::command_id)
	{
	}

	int params::size() const
	{
		return game::cmd_argc[this->nesting_];
	}

	const char* params::get(const int index) const
	{
		if (index >= this->size())
		{
			return "";
		}

		return game::cmd_argv[this->nesting_][index];
	}

	std::string params::join(const int index) const
	{
		std::string result = {};

		for (auto i = index; i < this->size(); i++)
		{
			if (i > index) result.append(" ");
			result.append(this->get(i));
		}
		return result;
	}

	std::vector<std::string> params::get_all() const
	{
		std::vector<std::string> params_;
		for (auto i = 0; i < this->size(); i++)
		{
			params_.push_back(this->get(i));
		}
		return params_;
	}

	void add_raw(const char* name, void (*callback)())
	{
		game::Cmd_AddCommandInternal(name, callback, cmd_allocator.allocate<game::cmd_function_s>());
	}

	void add(const char* name, const std::function<void(const params&)>& callback)
	{
		const auto command = utils::string::to_lower(name);

		if (handlers.find(command) == handlers.end())
			add_raw(name, main_handler);

		handlers[command] = callback;
	}

	void add(const char* name, const std::function<void()>& callback)
	{
		add(name, [callback](const params&)
		{
			callback();
		});
	}

	void execute(std::string command)
	{
		command += "\n";
		game::Cbuf_AddText(0, command.data());
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			scheduler::once([&]()
				{
					add("marco", []()
					{
						printf("polo\n");
					});

					add("dvarDump", [](const params& argument)
					{
						std::string filename;
						if (argument.size() == 2)
						{
							filename = "consolation/";
							filename.append(argument.get(1));
							if (!filename.ends_with(".txt"))
							{
								filename.append(".txt");
							}
						}

						console::info("================================ DVAR DUMP ========================================\n");
						for (auto i = 0; i < *game::dvarCount; i++)
						{
							auto* dvar = game::sortedDvars[i];
							
							if (dvar)
							{
								if (!filename.empty())
								{
									try 
									{
										const auto line = std::format("{} \"{}\"\r\n", dvar->name, dvars::Dvar_ValueToString(dvar, dvar->current));
										utils::io::write_file(filename, line, i != 0);
									}
									catch (...)
									{

									}
								}

								try
								{
									console::info("%s \"%s\"\n", dvar->name, dvars::Dvar_ValueToString(dvar, dvar->current));
								}
								catch (...)
								{

								}
							}
						}

						console::info("\n%i dvars\n", *game::dvarCount);
						console::info("================================ END DVAR DUMP ====================================\n");
					});

					add("commandDump", [](const params& argument)
						{
							console::info("================================ COMMAND DUMP =====================================\n");
							game::cmd_function_s* cmd = (*game::cmd_functions);
							std::string filename;
							if (argument.size() == 2)
							{
								filename = "consolation/";
								filename.append(argument.get(1));
								if (!filename.ends_with(".txt"))
								{
									filename.append(".txt");
								}
							}
							int i = 0;
							while (cmd)
							{
								if (cmd->name)
								{
									if (!filename.empty())
									{
										const auto line = std::format("{}\r\n", cmd->name);
										utils::io::write_file(filename, line, i != 0);
									}
									console::info("%s\n", cmd->name);
									i++;
								}
								cmd = cmd->next;
							}
							console::info("\n%i commands\n", i);
							console::info("================================ END COMMAND DUMP =================================\n");
						});
					add("listassetpool", [](const params& params)
						{
							if (params.size() < 2)
							{
								console::info("listassetpool <poolnumber> [filter]: list all the assets in the specified pool\n");

								for (auto i = 0; i < game::XAssetType::ASSET_TYPE_COUNT; i++)
								{
									console::info("ASSET; %d %s\n", i, game::g_assetNames[i]);
								}
							}
							else
							{
								const auto type = static_cast<game::XAssetType>(atoi(params.get(1)));
								if (type < 0 || type >= game::XAssetType::ASSET_TYPE_COUNT)
								{
									console::info("Invalid pool passed must be between [0, %d]\n", game::XAssetType::ASSET_TYPE_COUNT - 1);
									return;
								}

								console::info("Listing assets in pool %s\n", game::g_assetNames[type]);

								auto total_assets = 0;
								const std::string filter = params.get(2);

								fastfiles::enum_assets(type, [type, filter](const game::XAssetHeader header)
									{
										auto asset = game::XAsset{ type, header };
										const auto* const asset_name = game::DB_GetXAssetName(&asset);

										if (!filter.empty() && !console::match_compare(filter, asset_name, false))
										{
											return;
										}

										console::info("%s\n", asset_name);
									}, true);
							}
						});
				}, scheduler::main);
		}

		void pre_destroy() override
		{
			cmd_allocator.clear();
		}
	};
}

REGISTER_COMPONENT(command::component)
