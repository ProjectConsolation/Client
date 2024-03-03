#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "filesystem.hpp"
#include "gsc.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

namespace gsc
{
	namespace
	{
		std::unordered_map<std::string, game::scr_function_t> scr_functions;
		std::unordered_map<std::string, game::scr_method_t> scr_methods;

		std::unordered_map<std::string, std::uint32_t> main_handles;
		std::unordered_map<std::string, std::uint32_t> init_handles;

		std::unordered_map<const char*, const char*> replaced_functions;
		const char* replaced_pos = nullptr;

		std::unordered_map<std::string, game::RawFile*> loaded_scripts;

		game::method_t player_get_method_stub(const char** name)
		{
			const auto got = scr_methods.find(*name);
			if (got != scr_methods.end())
			{
				return got->second.call;
			}

			return utils::hook::invoke<game::method_t>(game::game_offset(0x1015AD00), name);
		}

		game::function_t scr_get_function_stub(const char** name, int* type)
		{
			const auto got = scr_functions.find(*name);
			if (got != scr_functions.end())
			{
				*type = got->second.developer;
				return got->second.call;
			}

			return utils::hook::invoke<game::function_t>(game::game_offset(0x10197460), name, type);
		}

		void load_script(const std::string& name)
		{
			const auto custom_script_name = name + ".gsc"s;

			auto script = game::Scr_LoadScript(name.data());
			if (!script)
			{
				return;
			}

			const auto main_handle = game::Scr_GetFunctionHandle(name.data(), "main");
			const auto init_handle = game::Scr_GetFunctionHandle(name.data(), "init");

			if (main_handle)
			{
				console::debug("Loaded '%s::main'\n", name.data());
				main_handles[name] = main_handle;
			}

			if (init_handle)
			{
				console::debug("Loaded '%s::init'\n", name.data());
				init_handles[name] = init_handle;
			}
		}

		void load_scripts(const std::filesystem::path& root_dir, const std::filesystem::path& subfolder)
		{
			std::filesystem::path script_dir = root_dir / subfolder;
			if (!utils::io::directory_exists(script_dir.generic_string()))
			{
				return;
			}

			const auto scripts = utils::io::list_files(script_dir.generic_string());
			for (const auto& script : scripts)
			{
				if (!script.ends_with(".gsc"))
				{
					continue;
				}

				std::filesystem::path path(script);
				const auto relative = path.lexically_relative(root_dir).generic_string();
				const auto base_name = relative.substr(0, relative.size() - 4);

				load_script(base_name);
			}
		}

		void load_gametype_script_stub()
		{
			utils::hook::invoke<void>(game::game_offset(0x101A8A70));

			for (const auto& path : filesystem::get_search_paths())
			{
				load_scripts(path, "scripts/");
				load_scripts(path, "scripts/mp/");
			}
		}

		void g_load_structs_stub()
		{
			for (auto& function_handle : main_handles)
			{
				console::debug("Executing '%s::main'\n", function_handle.first.data());
				game::RemoveRefToObject(game::Scr_ExecThread(function_handle.second));
			}

			utils::hook::invoke<void>(game::game_offset(0x10188010));
		}

		void save_reigstered_weapons_stub()
		{
			for (auto& function_handle : init_handles)
			{
				console::debug("Executing '%s::init'\n", function_handle.first.data());
				game::RemoveRefToObject(game::Scr_ExecThread(function_handle.second));
			}

			utils::hook::invoke<void>(game::game_offset(0x10179920));
		}

		bool read_raw_script_file(const std::string& name, std::string* data)
		{
			if (filesystem::read_file(name, data))
			{
				return true;
			}

			/*
			const auto* name_str = name.data();
			if (game::DB_XAssetExists(game::ASSET_TYPE_RAWFILE, name_str) &&
				!game::DB_IsXAssetDefault(game::ASSET_TYPE_RAWFILE, name_str))
			{
				const auto asset = game::DB_FindXAssetHeader(game::ASSET_TYPE_RAWFILE, name_str, false);
				const auto len = game::DB_GetRawFileLen(asset.rawfile);
				data->resize(len);
				game::DB_GetRawBuffer(asset.rawfile, data->data(), len);
				if (len > 0)
				{
					data->pop_back();
				}

				return true;
			}
			*/

			return false;
		}

		game::RawFile* load_custom_script(const char* file_name)
		{
			if (const auto itr = loaded_scripts.find(file_name); itr != loaded_scripts.end())
			{
				return itr->second;
			}

			std::string source_buffer{};
			if (!read_raw_script_file(file_name, &source_buffer) || source_buffer.empty())
			{
				return nullptr;
			}

			const auto rawfile_ptr = utils::memory::allocate<game::RawFile>();
			rawfile_ptr->name = file_name;

			auto buffer_size = source_buffer.size();
			rawfile_ptr->buffer = static_cast<char*>(utils::memory::allocate(buffer_size + 1));
			std::memcpy(rawfile_ptr->buffer, source_buffer.data(), buffer_size);

			loaded_scripts[file_name] = rawfile_ptr;

			console::debug("Loaded custom gsc '%s\n", file_name);

			return rawfile_ptr;
		}

		game::RawFile* find_script(game::XAssetType type, const char* name, int allow_create_default)
		{
			auto* script = load_custom_script(name);
			if (script)
			{
				return script;
			}

			const auto rawfile = game::DB_FindXAssetHeader_Internal(type, name, allow_create_default).rawfile;
#ifdef GSC_DUMPING
			utils::io::write_file(utils::string::va("gsc_dump/%s", name), rawfile->buffer);
			console::debug("Dumped %s\n", name);
#endif
			return rawfile;
		}

		const char* get_code_pos_for_param(int index)
		{
			if (static_cast<unsigned int>(index) >= game::scrVmPub->outparamcount)
			{
				utils::hook::invoke<void>(game::game_offset(0x10235640), static_cast<unsigned int>(index), "get_code_pos_for_param: Index is out of range!\n");
				return "";
			}

			const auto value = &game::scrVmPub->top[-index];

			if (value->type != game::VAR_FUNCTION)
			{
				utils::hook::invoke<void>(game::game_offset(0x10235640), static_cast<unsigned int>(index), "get_code_pos_for_param: Invalid type! (Expects a function as parameter)\n");
				return "";
			}

			return value->u.codePosValue;
		}

		void get_replaced_pos(const char* pos)
		{
			if (replaced_functions.contains(pos))
			{
				replaced_pos = replaced_functions[pos];
			}
		}

		void set_replaced_pos(const char* what, const char* with)
		{
			if (what[0] == '\0' || with[0] == '\0')
			{
				console::warn("Invalid parameters passed to set_replaced_pos\n");
				return;
			}

			if (replaced_functions.contains(what))
			{
				console::warn("replaced_functions already contains codePosValue for a function\n");
			}

			replaced_functions[what] = with;
		}

		void vm_execute_stub()
		{
			auto dword_116377F8 = game::game_offset(0x116377F8);
			auto qword_11738440 = game::game_offset(0x11738440);

			auto jmp_back_to_10237866 = game::game_offset(0x10237866);

			__asm
			{
				pushad

				push edx
				call get_replaced_pos
				pop edx
				popad

				cmp replaced_pos, 0
				jne set_pos

				movzx eax, byte ptr[edx]
				inc edx

				jmp loc_1
				loc_1 :
				cmp eax, 0x86

					mov dword_116377F8, eax
					mov dword ptr qword_11738440, edx

					push jmp_back_to_10237866
					retn
					set_pos :
				mov edx, replaced_pos
					mov replaced_pos, 0

					movzx eax, byte ptr[edx]
					inc edx

					jmp loc_1
			}
		}
	}

	void add_function(const char* name, game::function_t func, int type)
	{
		game::scr_function_t function;
		function.call = func;
		function.developer = type;

		scr_functions[utils::string::to_lower(name)] = function;
	}

	void add_method(const char* name, game::method_t func, int type)
	{
		game::scr_method_t method;
		method.call = func;
		method.developer = type;

		scr_methods[utils::string::to_lower(name)] = method;
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			// custom gsc functions/methods
			//utils::hook::call(game::game_offset(0x10197711), player_get_method_stub);
			//utils::hook::call(game::game_offset(0x10229B64), scr_get_function_stub);

			// load custom scripts + override scripts
			utils::hook::call(game::game_offset(0x101A8ED4), load_gametype_script_stub); // load our custom gsc

			// execute handles
			utils::hook::call(game::game_offset(0x101AD1FA), g_load_structs_stub); // execute main handles (Scr_LoadGameType is inlined)
			utils::hook::call(game::game_offset(0x101AD27A), save_reigstered_weapons_stub); // execute init handles (Scr_StartupGameType is inlined)

			// hook xasset function to return our own scripts
			utils::hook::call(game::game_offset(0x1022DC10), find_script);

			// hook vm_execute to redirect function calls
			//utils::hook::jump(game::game_offset(0x1023784C), vm_execute_stub);

			add_function("replacefunc", []()
				{
					if (game::Scr_GetNumParam() != 2)
					{
						//game::Scr_Error("replacefunc: two parameters are required.\n");
						console::error("replacefunc: two parameters are required.\n");
						return;
					}

					const auto what = get_code_pos_for_param(0);
					const auto with = get_code_pos_for_param(1);

					set_replaced_pos(what, with);
				});

			// reset replaced functions on game shutdown
			scheduler::on_shutdown([]
				{
					main_handles.clear();
					init_handles.clear();
					replaced_functions.clear();
					loaded_scripts.clear();
				});
		}
	};
}

REGISTER_COMPONENT(gsc::component)
