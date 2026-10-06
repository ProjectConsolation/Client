#include <std_include.hpp>

#include "component/engine/console/console.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

namespace game
{
	HMODULE mp_dll = nullptr;

	uintptr_t game_offset(uintptr_t ida_address)
	{
		if (mp_dll == nullptr)
		{
			mp_dll = GetModuleHandleA("jb_mp_s.dll");
			assert(mp_dll != nullptr);
		}

		return ida_address - 0x10000000 + reinterpret_cast<uintptr_t>(mp_dll);
	}

	void Cbuf_AddText(int controller, const char* text)
	{
		int func_loc = game_offset(0x103F5180);

		__asm
		{
			mov eax, text
			mov ecx, controller
			call func_loc
		}
	}

	void CL_KeyEvent(int localClientNum, int key, int down, unsigned int time)
	{
		int func_loc = game_offset(0x1031A680);

		__asm
		{
			push time
			push down
			push key
			mov ecx, localClientNum
			call func_loc
			add esp, 0xC
		}
	}

	// half of this is inlined on QoS, so just re-writing it all since its little work
	cmd_function_s* Cmd_FindCommand(const char* name)
	{
		cmd_function_s* command;

		for (command = *cmd_functions; command; command = command->next)
		{
			if (!strcmp(name, command->name))
			{
				return command;
			}
		}

		return 0;
	}

	void Cmd_AddCommandInternal(const char* name, void(__cdecl* function)(), cmd_function_s* cmd)
	{
		if (Cmd_FindCommand(name))
		{
			if (function)
			{
				console::error("Cmd_AddCommand: %s already defined\n", name);
			}
		}
		else
		{
			cmd->name = name;
			cmd->function = function;
			cmd->next = *cmd_functions;
			*cmd_functions = cmd;
			console::debug("registered cmd '%s'\n", name);
		}
	}

	//credits to IW3SP--MOD
	DB_XAssetGetNameHandler_t* DB_XAssetGetNameHandler = reinterpret_cast<DB_XAssetGetNameHandler_t*>(game_offset(0x1055E470)); //why the fuck do i need this here specifically?

	const char* DB_GetXAssetName(game::XAsset* asset)
	{
		if (!asset) return "";

		assert(asset->header.data);

		return game::DB_XAssetGetNameHandler[asset->type](&asset->header);
	}

	bool DB_IsXAssetDefault(XAssetType type, const char* name)
	{
		int func_loc = game_offset(0x103DFC00);
		bool answer = false;
		int type_ = static_cast<int>(type);

		__asm
		{
			push ebx
			mov edi, type_
			call func_loc
			add esp, 4
			mov answer, al
		}

		return answer;
	}

	void DB_EnumXAssetEntries(XAssetType type, std::function<void(XAssetEntryPoolEntry*)> callback, bool overrides)
	{
		volatile long* lock = reinterpret_cast<volatile long*>(game_offset(0x1056250A));
		InterlockedIncrement(lock);

		while (*reinterpret_cast<volatile long*>(game_offset(0x105624F8))) std::this_thread::sleep_for(1ms);

		unsigned int index = 0;
		do
		{
			unsigned short hashIndex = game::db_hashTable[index];
			if (hashIndex)
			{
				do
				{
					XAssetEntryPoolEntry* asset = &g_assetEntryPool[hashIndex];
					hashIndex = asset->entry.nextHash;
					if (asset->entry.asset.type == type)
					{
						callback(asset);
						if (overrides)
						{
							unsigned short overrideIndex = asset->entry.nextOverride;
							if (asset->entry.nextOverride)
							{
								do
								{
									asset = &g_assetEntryPool[overrideIndex];
									callback(asset);
									overrideIndex = asset->entry.nextOverride;
								} while (overrideIndex);
							}
						}
					}
				} while (hashIndex);
			}
			++index;
		} while (index < 0x9C40);
		InterlockedDecrement(lock);
	}

	unsigned int Scr_GetFunctionHandle(const char* filename, const char* funcHandle)
	{
		int func_loc = game::game_offset(0x1022E410);
		unsigned int answer = 0;

		__asm
		{
			push funcHandle;
			mov eax, filename;
			call func_loc;
			add esp, 4;
			mov answer, eax;
		}

		return answer;
	}

	void RemoveRefToObject(unsigned int obj)
	{
		int func_loc = game::game_offset(0x10230DB0);

		__asm
		{
			mov edi, obj;
			call func_loc;
		}
	}

	__int16 Scr_ExecThread(int handle)
	{
		int func_loc = game::game_offset(0x1023B960);
		__int16 answer = 0;

		__asm
		{
			mov eax, handle;
			call func_loc;
			mov answer, ax;
		}

		return answer;
	}

	int Scr_LoadScript(const char* name)
	{
		int func_loc = game::game_offset(0x1022E7C0);
		int answer = 0;

		__asm
		{
			mov ecx, name;
			call func_loc;
			mov answer, eax;
		}

		return answer;
	}

	ScreenPlacement ScrPlace_GetViewPlacement()
	{
		return *game::scrPlaceView;
	}

	Font_s* R_RegisterFont(const char* font)
	{
		return DB_FindXAssetHeader(ASSET_TYPE_FONT, font).font;
	}

	Material* Material_RegisterHandle(const char* material)
	{
		return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, material).material;
	}

	dvar_s* Dvar_FindMalleableVar(const char* dvarName)
	{
		// QoS 1.1: native lookup holds the dvar lock and invokes the hash
		// helper with its name in EAX. That helper is not a cdecl function.
		using find_var_t = dvar_s* (__cdecl*)(const char*);
		return dvarName ? reinterpret_cast<find_var_t>(game_offset(0x10276040))(dvarName) : nullptr;
	}

	dvar_s* Dvar_FindVar(const char* dvarName)
	{
		return Dvar_FindMalleableVar(dvarName);
	}

	dvar_s* Dvar_RegisterVariant(const char* name, DvarType type, unsigned short flags,
		const char* description, int unknown, DvarValue value, DvarLimits domain)
	{
		// QoS 1.1 0x10278960: name in EDI; remaining arguments on the stack.
		// Like KisakCOD Dvar_RegisterVariant, this re-registers an existing
		// variable (including user-created strings) rather than allocating duplicates.
		static_assert(sizeof(DvarValue) == 16);
		static_assert(sizeof(DvarLimits) == 8);
		const auto function = game_offset(0x10278960);
		const int native_type = type;
		const int native_flags = flags;
		const auto* value_words = reinterpret_cast<const unsigned int*>(&value);
		const auto* domain_words = reinterpret_cast<const unsigned int*>(&domain);
		dvar_s* result;
		__asm
		{
			push unknown
			push description
			mov eax, domain_words
			push dword ptr [eax + 4]
			push dword ptr [eax]
			mov eax, value_words
			push dword ptr [eax + 12]
			push dword ptr [eax + 8]
			push dword ptr [eax + 4]
			push dword ptr [eax]
			push native_flags
			push native_type
			mov edi, name
			call function
			add esp, 40
			mov result, eax
		}
		return result;
	}

	void Dvar_SetString(const char* dvarName, const char* value)
	{
		int func_loc = game::game_offset(0x10278FD0);

		__asm
		{
			mov eax, dvarName
			mov edi, value
			call func_loc
		}
	}

	void GamerProfile_UpdateProfileFromDvars(int profileIndex, int updateState)
	{
		int func_loc = game::game_offset(0x102439B0);

		__asm
		{
			push updateState
			push profileIndex
			call func_loc
			add esp, 0x8
		}
	}

	void Live_UpdateClan(int object, const char* clan)
	{
		int func_loc = game::game_offset(0x10324960);

		__asm
		{
			push clan
			mov eax, object
			call func_loc
			add esp, 0x4
		}
	}

	dvar_s* Dvar_RegisterBool(const char* name, bool value, int flags, const char* desc)
	{
		int func_loc = game::game_offset(0x10278E60);
		dvar_s* answer = nullptr;

		__asm
		{
			push flags
			push value
			push name
			mov edx, desc
			call func_loc;
			mov answer, eax;
			add esp, 0xC;
			mov answer, eax
		}

		return answer;
	}

	unsigned int Scr_GetNumParam()
	{
		return *scr_numParams;
	}
}
