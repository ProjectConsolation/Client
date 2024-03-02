#pragma once

#include "structs.hpp"

namespace game
{
	extern HMODULE mp_dll;

	uintptr_t game_offset(uintptr_t ida_address);

	template <typename T>
	class symbol
	{
	public:
		symbol(const size_t mp_address)
			: mp_object_(reinterpret_cast<T*>(mp_address))
		{
		}

		T* get() const
		{
			return reinterpret_cast<T*>((uint64_t)mp_object_);
		}

		operator T* () const
		{
			return this->get();
		}

		T* operator->() const
		{
			return this->get();
		}

	private:
		T* mp_object_;
	};

	void Cbuf_AddText(int controller, const char* text);
	void Cmd_AddCommandInternal(const char* name, void(__cdecl* function)(), cmd_function_s* cmd);
	bool DB_IsXAssetDefault(XAssetType type, const char* name);
	void DB_EnumXAssetEntries(XAssetType type, std::function<void(XAssetEntryPoolEntry*)> callback, bool overrides);
	ScreenPlacement ScrPlace_GetViewPlacement();
	Font_s* R_RegisterFont(const char* font);
	Material* Material_RegisterHandle(const char* material);
	dvar_s* Dvar_FindVar(const char* dvarName);

	unsigned int Scr_GetFunctionHandle(const char* filename, const char* funcHandle);
	void RemoveRefToObject(unsigned int obj);
	__int16 Scr_ExecThread(int handle);

	int Scr_LoadScript(const char* name);
}

#include "symbols.hpp"
