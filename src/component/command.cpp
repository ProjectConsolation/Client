#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "scheduler.hpp"

#include <utils/memory.hpp>
#include <utils/string.hpp>

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
		game::Cmd_AddCommandInternal(name, callback, cmd_allocator.allocate<game::qos::cmd_function_s>());
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
			}, scheduler::main);
		}

		void pre_destroy() override
		{
			cmd_allocator.clear();
		}
	};
}

REGISTER_COMPONENT(command::component)
