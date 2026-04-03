#include <std_include.hpp>
#include "component_loader.hpp"

namespace
{
	void log_component_event(const std::string& stage, const std::string& component_name)
	{
		const auto line = std::format("[component_loader] {} {}\n", stage, component_name);
		OutputDebugStringA(line.c_str());
		std::printf("%s", line.c_str());
		std::fflush(stdout);
	}
}

void component_loader::register_component(std::unique_ptr<component_interface>&& component_)
{
	get_components().push_back(std::move(component_));
}

bool component_loader::post_start()
{
	static auto handled = false;
	if (handled) return true;
	handled = true;

	try
	{
		for (const auto& component_ : get_components())
		{
			const auto component_name = std::string(typeid(*component_).name());
			log_component_event("post_start", component_name);

			try
			{
				component_->post_start();
			}
			catch (premature_shutdown_trigger&)
			{
				throw;
			}
			catch (const std::exception& error)
			{
				throw std::runtime_error(std::format("post_start failed in {}: {}", component_name, error.what()));
			}
			catch (const char* error)
			{
				throw std::runtime_error(std::format("post_start failed in {}: {}", component_name, error));
			}
			catch (...)
			{
				throw std::runtime_error(std::format("post_start failed in {}: unknown exception", component_name));
			}
		}
	}
	catch (premature_shutdown_trigger&)
	{
		return false;
	}

	return true;
}

bool component_loader::post_load()
{
	static auto handled = false;
	if (handled) return true;
	handled = true;

	clean();

	try
	{
		for (const auto& component_ : get_components())
		{
			const auto component_name = std::string(typeid(*component_).name());
			log_component_event("post_load", component_name);

			try
			{
				component_->post_load();
			}
			catch (premature_shutdown_trigger&)
			{
				throw;
			}
			catch (const std::exception& error)
			{
				throw std::runtime_error(std::format("post_load failed in {}: {}", component_name, error.what()));
			}
			catch (const char* error)
			{
				throw std::runtime_error(std::format("post_load failed in {}: {}", component_name, error));
			}
			catch (...)
			{
				throw std::runtime_error(std::format("post_load failed in {}: unknown exception", component_name));
			}
		}
	}
	catch (premature_shutdown_trigger&)
	{
		return false;
	}

	return true;
}

void component_loader::pre_destroy()
{
	static auto handled = false;
	if (handled) return;
	handled = true;

	for (const auto& component_ : get_components())
	{
		component_->pre_destroy();
	}
}

void component_loader::clean()
{
	auto& components = get_components();
	for (auto i = components.begin(); i != components.end();)
	{
		if (!(*i)->is_supported())
		{
			(*i)->pre_destroy();
			i = components.erase(i);
		}
		else
		{
			++i;
		}
	}
}

void* component_loader::load_import(const std::string& library, const std::string& function)
{
	void* function_ptr = nullptr;

	for (const auto& component_ : get_components())
	{
		auto* const component_function_ptr = component_->load_import(library, function);
		if (component_function_ptr)
		{
			function_ptr = component_function_ptr;
		}
	}

	return function_ptr;
}

void component_loader::trigger_premature_shutdown()
{
	throw premature_shutdown_trigger();
}

std::vector<std::unique_ptr<component_interface>>& component_loader::get_components()
{
	using component_vector = std::vector<std::unique_ptr<component_interface>>;
	using component_vector_container = std::unique_ptr<component_vector, std::function<void(component_vector*)>>;

	static component_vector_container components(new component_vector, [](component_vector* component_vector)
	{
		pre_destroy();
		delete component_vector;
	});

	return *components;
}
