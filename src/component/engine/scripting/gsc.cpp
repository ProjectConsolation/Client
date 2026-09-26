#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/console.hpp"
#include "filesystem.hpp"
#include "gsc.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

#include <algorithm>
#include <cctype>

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

		struct loaded_script
		{
			game::RawFile* rawfile{};
			std::string source{};
			std::string real_path{};
		};

		struct watched_script
		{
			std::string script_name{};
			std::string file_name{};
			std::string real_path{};
			std::string source{};
			std::filesystem::file_time_type write_time{};
			std::unordered_map<std::string, std::vector<const char*>> function_positions{};
		};

		std::unordered_map<std::string, loaded_script> loaded_scripts;
		std::unordered_map<std::string, std::string> hot_reload_sources;
		std::unordered_map<std::string, watched_script> watched_scripts;
		std::uint32_t hot_reload_generation{};
		bool scripts_ready{};
		utils::hook::detour free_scripts_hook;

		bool& script_loading()
		{
			return *reinterpret_cast<bool*>(game::game_offset(0x118D5250));
		}

		const char* get_program_buffer()
		{
			return *reinterpret_cast<const char**>(game::game_offset(0x1173CBF0));
		}

		game::RawFile* make_rawfile(const char* file_name, const std::string& source_buffer)
		{
			const auto rawfile_ptr = utils::memory::allocate<game::RawFile>();
			const auto name_len = std::strlen(file_name);
			rawfile_ptr->name = static_cast<char*>(utils::memory::allocate(name_len + 1));
			std::memcpy(const_cast<char*>(rawfile_ptr->name), file_name, name_len);
			const_cast<char*>(rawfile_ptr->name)[name_len] = '\0';

			const auto buffer_size = source_buffer.size();
			rawfile_ptr->len = static_cast<unsigned int>(buffer_size + 1);
			rawfile_ptr->buffer = static_cast<char*>(utils::memory::allocate(buffer_size + 1));
			std::memcpy(rawfile_ptr->buffer, source_buffer.data(), buffer_size);
			rawfile_ptr->buffer[buffer_size] = '\0';
			return rawfile_ptr;
		}

		std::vector<std::string> get_function_names(const std::string& source)
		{
			std::string code = source;
			bool in_string = false;
			bool in_line_comment = false;
			bool in_block_comment = false;
			bool escaped = false;

			for (std::size_t i = 0; i < code.size(); ++i)
			{
				const auto current = code[i];
				const auto next = i + 1 < code.size() ? code[i + 1] : '\0';

				if (in_line_comment)
				{
					if (current == '\n') in_line_comment = false;
					else code[i] = ' ';
					continue;
				}

				if (in_block_comment)
				{
					code[i] = ' ';
					if (current == '*' && next == '/')
					{
						code[++i] = ' ';
						in_block_comment = false;
					}
					continue;
				}

				if (in_string)
				{
					code[i] = ' ';
					if (!escaped && current == '"') in_string = false;
					escaped = !escaped && current == '\\';
					continue;
				}

				if (current == '/' && next == '/')
				{
					code[i] = code[++i] = ' ';
					in_line_comment = true;
				}
				else if (current == '/' && next == '*')
				{
					code[i] = code[++i] = ' ';
					in_block_comment = true;
				}
				else if (current == '"')
				{
					code[i] = ' ';
					in_string = true;
					escaped = false;
				}
			}

			std::vector<std::string> names{};
			int brace_depth = 0;
			for (std::size_t i = 0; i < code.size();)
			{
				if (code[i] == '{')
				{
					++brace_depth;
					++i;
					continue;
				}
				if (code[i] == '}')
				{
					brace_depth = std::max(0, brace_depth - 1);
					++i;
					continue;
				}
				if (brace_depth != 0 || !(std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_'))
				{
					++i;
					continue;
				}

				const auto name_start = i++;
				while (i < code.size() && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) ++i;
				const auto name = code.substr(name_start, i - name_start);
				while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
				if (i >= code.size() || code[i] != '(') continue;

				int parenthesis_depth = 1;
				for (++i; i < code.size() && parenthesis_depth; ++i)
				{
					if (code[i] == '(') ++parenthesis_depth;
					else if (code[i] == ')') --parenthesis_depth;
				}
				while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
				if (parenthesis_depth == 0 && i < code.size() && code[i] == '{')
				{
					names.push_back(utils::string::to_lower(name));
				}
			}

			std::sort(names.begin(), names.end());
			names.erase(std::unique(names.begin(), names.end()), names.end());
			return names;
		}

		void capture_function_positions(watched_script& script, const std::string& compiled_name,
			const std::string& source, const bool replace_existing)
		{
			const auto* program_buffer = get_program_buffer();
			if (!program_buffer) return;

			for (const auto& function_name : get_function_names(source))
			{
				const auto handle = game::Scr_GetFunctionHandle(compiled_name.c_str(), function_name.c_str());
				if (!handle) continue;

				const auto* new_position = program_buffer + handle;
				auto& positions = script.function_positions[function_name];
				if (replace_existing)
				{
					for (const auto* old_position : positions)
					{
						replaced_functions[old_position] = new_position;
					}
				}
				positions.push_back(new_position);
			}
		}

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

		bool read_raw_script_file(const std::string& name, std::string* data, std::string* real_path)
		{
			if (filesystem::read_file(name, data, real_path))
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
				return itr->second.rawfile;
			}

			std::string source_buffer{};
			std::string real_path{};
			if (const auto source = hot_reload_sources.find(file_name); source != hot_reload_sources.end())
			{
				source_buffer = source->second;
			}
			else if (!read_raw_script_file(file_name, &source_buffer, &real_path))
			{
				return nullptr;
			}

			auto* rawfile_ptr = make_rawfile(file_name, source_buffer);
			loaded_scripts[file_name] = {rawfile_ptr, source_buffer, real_path};

			console::debug("Loaded custom gsc '%s'\n", file_name);

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

		void finish_script_loading_stub()
		{
			watched_scripts.clear();
			for (const auto& [file_name, loaded] : loaded_scripts)
			{
				if (loaded.real_path.empty() || !file_name.ends_with(".gsc")) continue;

				watched_script script{};
				script.script_name = file_name.substr(0, file_name.size() - 4);
				script.file_name = file_name;
				script.real_path = loaded.real_path;
				script.source = loaded.source;

				std::error_code error{};
				script.write_time = std::filesystem::last_write_time(script.real_path, error);
				capture_function_positions(script, script.script_name, script.source, false);
				watched_scripts.emplace(file_name, std::move(script));
			}

			// QoS normally tail-calls Scr_EndLoadScripts here. Retaining these four
			// compiler lookup objects lets later generations append bytecode to the
			// existing program hunk. Restore runtime error semantics while idle;
			// Scr_FreeScripts is hooked below so native cleanup still owns the data.
			script_loading() = false;
			scripts_ready = true;
		}

		void free_scripts_stub()
		{
			scripts_ready = false;
			if (*reinterpret_cast<std::uint32_t*>(game::game_offset(0x118B5238)))
			{
				script_loading() = true;
			}

			free_scripts_hook.invoke<void>();
		}

		void reload_script(watched_script& script, std::string source)
		{
			const auto generation = ++hot_reload_generation;
			const auto alias_name = std::string(utils::string::va("scripts/__consolation_hot_%08x", generation));
			const auto alias_file = alias_name + ".gsc";

			hot_reload_sources[alias_file] = source;
			console::info("reloading script file %s\n", script.file_name.c_str());

			script_loading() = true;
			const auto loaded = game::Scr_LoadScript(alias_name.c_str());
			script_loading() = false;
			if (!loaded)
			{
				console::warn("could not reload script file %s\n", script.file_name.c_str());
				return;
			}

			capture_function_positions(script, alias_name, source, true);
			script.source = std::move(source);
		}

		void poll_script_changes()
		{
			if (!scripts_ready) return;

			for (auto& [name, script] : watched_scripts)
			{
				std::error_code error{};
				const auto write_time = std::filesystem::last_write_time(script.real_path, error);
				if (error || write_time == script.write_time) continue;

				script.write_time = write_time;
				std::string source{};
				if (!utils::io::read_file(script.real_path, &source) || source == script.source) continue;
				reload_script(script, std::move(source));
			}
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
			auto opcode = game::game_offset(0x116377F8);
			auto code_pos = game::game_offset(0x11738440);

			auto resume = game::game_offset(0x10237866);

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
				mov edi, 1
				add edx, edi

				jmp store_state
				store_state:
				cmp eax, 0x86

					push ecx
					mov ecx, opcode
					mov dword ptr [ecx], eax
					mov ecx, code_pos
					mov dword ptr [ecx], edx
					pop ecx

					push resume
					retn
				set_pos:
				mov edx, replaced_pos
					mov replaced_pos, 0

					movzx eax, byte ptr[edx]
					mov edi, 1
					add edx, edi

					jmp store_state
			}
		}

		std::string format(va_list* ap, const char* message)
		{
			static thread_local char buffer[0x1000];

			const auto count = vsnprintf_s(buffer, _TRUNCATE, message, *ap);
			if (count < 0)
			{
				return {};
			}

			return { buffer, static_cast<size_t>(count) };
		}

		void Scr_PrintSourcePos(const char* a2, const char* a3, unsigned int a4)
		{
			const char* v4{}; // r30
			const char* v7{}; // r27
			int v8{}; // r31
			unsigned int i; // ctr
			signed int v10{}; // r3
			char* v11; // r5
			signed int v12; // ctr
			char v13; // r0
			int v14; // r0
			const char* v15; // r5
			const char* v16; // r3
			const char* v17; // r3
			int j; // r27
			char v20[1028]{}; // [sp+8h] [-418h] BYREF

			if (a3)
			{
				v7 = a3;
				v8 = 0;
				for (i = a4; i; --i)
				{
					if (!*a3)
					{
						v7 = a3 + 1;
						++v8;
					}
					++a3;
				}
				v4 = (const char*)(a3 - v7);
			}

			v10 = strlen(v7);
			if (v10 >= 1024)
				v10 = 1023;

			v11 = v20;
			v12 = v10;
			if (v10 > 0)
			{
				do
				{
					v13 = 32;
					if (*v7 != 9)
						v13 = *v7;
					*v11++ = v13;
					++v7;
					--v12;
				} while (v12);
			}
			if (v20[v10 - 1] == 13)
				v20[v10 - 1] = 0;

			v14 = *reinterpret_cast<int*>(game::game_offset(0x11738478)); //saveGame
			v20[v10] = 0;
			v15 = (const char*)"";
			if (v14)
				v15 = " (savegame)";

			v16 = (const char*)utils::string::va("(file '%s'%s, line %d)\n", a2, v15, v8 + 1);
			console::error("%s\n", v16);
			v17 = (const char*)utils::string::va("%s\n", v20);
			console::error("%s\n", v17);
			for (j = 0; j < (int)v4; ++j)
				console::error(" ");
		}

		utils::hook::detour compile_error_hook;
		void compile_error_stub(const char* fmt, ...)
		{
			unsigned int a1{};
			_asm
			{
				mov a1, esi;
			}
			va_list ap;
			va_start(ap, fmt);
			const auto result = format(&ap, fmt);
			va_end(ap);	

			console::error("\n");
			console::error("******* script compile error *******\n");
			console::error("%s: ", result.c_str());
			Scr_PrintSourcePos(*reinterpret_cast<char**>(game::game_offset(0x118B5224)), *reinterpret_cast<char**>(game::game_offset(0x118B522C)), a1);
			console::error("************************************\n");
			game::Com_Error((int)".\\gsc.cpp", 1467, 5, (char*)"script compile error\n%s\n%s\n(see console for details)\n", result.c_str(), "");
		}

		utils::hook::detour compile_error_2_hook;
		void compile_error_2_stub(const char* fmt, ...)
		{
			int a1{};
			_asm
			{
				mov a1, esi;
			}
			va_list ap;
			va_start(ap, fmt);
			const auto result = format(&ap, fmt);
			va_end(ap);

			
			console::error("\n");
			console::error("******* script compile error *******\n");
			console::error("%s: ", result.c_str());

			auto errorMessage = game::Scr_FormatCodePos((char*)a1, 0);
			console::error("%s", errorMessage);
			console::error("************************************\n");
			game::Com_Error((int)".\\gsc.cpp", 1507, 5, (char*)"script compile error\n%s\n%s\n(see console for details)\n", result.c_str(), errorMessage);
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

			compile_error_hook.create(game::game_offset(0x1022DD40), compile_error_stub);
			compile_error_2_hook.create(game::game_offset(0x1022DC70), compile_error_2_stub);

			// Keep the compiler's lookup objects alive after initial game-script loading.
			// The native shutdown path still owns and releases them with the program hunk.
			utils::hook::jump(game::game_offset(0x101A8EFF), finish_script_loading_stub);
			free_scripts_hook.create(game::game_offset(0x1022E4F0), free_scripts_stub);

			// Redirect calls from every older function generation to the latest one.
			utils::hook::jump(game::game_offset(0x1023784C), vm_execute_stub);
			scheduler::loop(poll_script_changes, scheduler::pipeline::main, 250ms);

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
				hot_reload_sources.clear();
				watched_scripts.clear();
				hot_reload_generation = 0;
				scripts_ready = false;
			});
		}
	};
}

REGISTER_COMPONENT(gsc::component)
