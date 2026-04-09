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
		constexpr int CLIENT_STRIDE = 688916;
		constexpr int CLIENT_REAL_PLAYER_OFFSET = 0x20;
		constexpr int CLIENT_USERINFO_OFFSET = 1604;
		constexpr int CLIENT_USERINFO_SIZE = 1024;
		constexpr auto BOT_NAMES_FILE = "consolation/bots.txt";

		std::unordered_map<std::string, std::function<void(params&)>> handlers;
		int next_bot_number = 1;
		std::vector<std::string> bot_names;

		std::uintptr_t get_clients_base()
		{
			return *reinterpret_cast<std::uintptr_t*>(game::game_offset(0x11CA5D8C));
		}

		int get_max_clients()
		{
			if (const auto* dvar = game::Dvar_FindVar("sv_maxclients"); dvar)
			{
				return std::max(dvar->current.integer, 1);
			}

			return 18;
		}

		std::uintptr_t get_client(int index)
		{
			return get_clients_base() + static_cast<std::uintptr_t>(index) * CLIENT_STRIDE;
		}

		bool is_bot_client(int index)
		{
			const auto client = get_client(index);
			return *reinterpret_cast<int*>(client) >= game::CS_CONNECTED
				&& *reinterpret_cast<int*>(client + CLIENT_REAL_PLAYER_OFFSET) == 0;
		}

		std::string get_info_value(const std::string& info, const std::string& key)
		{
			size_t pos = 0;
			while (pos < info.size())
			{
				if (info[pos] == '\\')
				{
					++pos;
				}

				const auto key_end = info.find('\\', pos);
				if (key_end == std::string::npos)
				{
					break;
				}

				const auto value_end = info.find('\\', key_end + 1);
				const auto current_key = info.substr(pos, key_end - pos);
				const auto current_value = info.substr(
					key_end + 1,
					(value_end == std::string::npos ? info.size() : value_end) - (key_end + 1)
				);

				if (current_key == key)
				{
					return current_value;
				}

				if (value_end == std::string::npos)
				{
					break;
				}

				pos = value_end;
			}

			return {};
		}

		void set_info_value(std::string& info, const std::string& key, const std::string& value)
		{
			std::vector<std::pair<std::string, std::string>> pairs;
			bool replaced = false;
			size_t pos = 0;

			while (pos < info.size())
			{
				if (info[pos] == '\\')
				{
					++pos;
				}

				const auto key_end = info.find('\\', pos);
				if (key_end == std::string::npos)
				{
					break;
				}

				const auto value_end = info.find('\\', key_end + 1);
				auto current_key = info.substr(pos, key_end - pos);
				auto current_value = info.substr(
					key_end + 1,
					(value_end == std::string::npos ? info.size() : value_end) - (key_end + 1)
				);

				if (current_key == key)
				{
					current_value = value;
					replaced = true;
				}

				pairs.emplace_back(std::move(current_key), std::move(current_value));

				if (value_end == std::string::npos)
				{
					break;
				}

				pos = value_end;
			}

			if (!replaced)
			{
				pairs.emplace_back(key, value);
			}

			std::string rebuilt;
			for (const auto& [current_key, current_value] : pairs)
			{
				rebuilt.push_back('\\');
				rebuilt.append(current_key);
				rebuilt.push_back('\\');
				rebuilt.append(current_value);
			}

			info = std::move(rebuilt);
		}

		std::string trim_bot_name(std::string name)
		{
			while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ' || name.back() == '\t'))
			{
				name.pop_back();
			}

			size_t start = 0;
			while (start < name.size() && (name[start] == ' ' || name[start] == '\t'))
			{
				++start;
			}

			return name.substr(start);
		}

		void load_bot_names()
		{
			bot_names.clear();

			std::string data;
			if (!utils::io::read_file(BOT_NAMES_FILE, &data))
			{
				return;
			}

			size_t start = 0;
			while (start <= data.size())
			{
				const auto end = data.find('\n', start);
				auto line = data.substr(start, end == std::string::npos ? data.size() - start : end - start);
				line = trim_bot_name(std::move(line));

				if (!line.empty())
				{
					bot_names.emplace_back(std::move(line));
				}

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}
		}

		std::string get_next_bot_name()
		{
			if (!bot_names.empty())
			{
				const auto index = static_cast<size_t>((next_bot_number - 1) % bot_names.size());
				++next_bot_number;
				return bot_names[index];
			}

			return std::format("consolation_bot{}", next_bot_number++ - 1);
		}

		int rename_new_bot_client()
		{
			for (int i = 0; i < get_max_clients(); ++i)
			{
				if (!is_bot_client(i))
				{
					continue;
				}

				auto* const userinfo = reinterpret_cast<char*>(get_client(i) + CLIENT_USERINFO_OFFSET);
				const std::string info = userinfo;
				if (get_info_value(info, "name").rfind("bot", 0) != 0)
				{
					continue;
				}

				auto updated = info;
				set_info_value(updated, "name", get_next_bot_name());
				set_info_value(updated, "clanAbbrev", "CSL");

				std::memset(userinfo, 0, CLIENT_USERINFO_SIZE);
				std::memcpy(userinfo, updated.data(), std::min(updated.size(), static_cast<size_t>(CLIENT_USERINFO_SIZE - 1)));
				game::SV_ClientUserinfoChanged(i);
				return i;
			}

			return -1;
		}

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
					load_bot_names();

					/*add("kick", [](const params& argument)
						{
							if (argument.size() < 2)
							{
								console::info("usage: kick <name>, <reason>(optional)\n");
								return;
							}
							std::string reason;
							if (reason.empty())
							{
								reason = "EXE_PLAYERKICKED";
							}

							game::SV_GameSendServerCommand(i, "r " + reason + "");
						});*/

					add("addbot", [](const params& args)
						{
							if (!game::SV_AddTestClient)
							{
								console::error("addbot: server not initialised\n");
								return;
							}

							load_bot_names();

							const int count = (args.size() >= 2) ? std::atoi(args[1]) : 1;
							if (count <= 0)
							{
								console::info("usage: addbot [count]\n");
								return;
							}

							int spawned = 0;
							for (int i = 0; i < count; ++i)
							{
								if (!game::SV_AddTestClient())
								{
									console::warn("addbot: server full after %i bot(s)\n", spawned);
									break;
								}

								const auto renamed_slot = rename_new_bot_client();
								if (renamed_slot < 0)
								{
									console::warn("addbot: spawned bot but failed to update its userinfo\n");
								}

								++spawned;
							}

							if (spawned > 0)
								console::info("addbot: spawned %i bot(s)\n", spawned);
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
									// TODO: fix this, there's a empty dvar (or multiple) caused string format crash which crashes game
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
