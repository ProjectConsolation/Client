#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"

#include <utils/memory.hpp>
#include <utils/string.hpp>
#include <utils/io.hpp>

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
					add("hello", []()
						{
							printf("hello from Project: Consolation!\n");
						});

					/*add("dvarDump", [](const params& argument)
						{
							std::string filename;
							if (argument.size() == 2)
							{
								filename = "consolation/";
								filename.append(argument[1]);
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
										const auto line = std::format("{} \"{}\"\r\n", dvar->name, game::Dvar_ValueToString(dvar, dvar->current));
										utils::io::write_file(filename, line, i != 0);
									}

									console::info("%s \"%s\"\n", dvar->name, game::Dvar_ValueToString(dvar, dvar->current));
								}
							}

							console::info("\n%i dvars\n", *game::dvarCount);
							console::info("================================ END DVAR DUMP ====================================\n");
						});*/

					add("commandDump", [](const params& argument)
						{
							console::info("================================ COMMAND DUMP =====================================\n");
							game::cmd_function_s* cmd = (*game::cmd_functions);
							std::string filename;
							/*if (argument.size() == 2)
							{
								filename = "Consolation/";
								filename.append(argument[1]);
								if (!filename.ends_with(".txt"))
								{
									filename.append(".txt");
								}
							}*/
							int i = 0;
							while (cmd)
							{
								if (cmd->name)
								{
									/*if (!filename.empty())
									{
										const auto line = std::format("{}\r\n", cmd->name);
										//utils::io::write_file(filename, line, i != 0);
									}*/
									console::info("%s\n", cmd->name);
									i++;
								}
								cmd = cmd->next;
							}
							console::info("\n%i commands\n", i);
							console::info("================================ END COMMAND DUMP =================================\n");
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
