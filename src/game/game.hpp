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
	void Cmd_AddCommandInternal(const char* name, void(__cdecl* function)(), qos::cmd_function_s* cmd);
	bool DB_IsXAssetDefault(qos::XAssetType type, const char* name);
	void DB_EnumXAssetEntries(qos::XAssetType type, std::function<void(qos::XAssetEntryPoolEntry*)> callback, bool overrides);
	const char* DB_GetXAssetName(game::qos::XAsset* asset);
	bool Key_IsCatcherActive(int mask);
	void R_AddCmdDrawTextASM(const char* text, int max_chars, void* font, float x, float y, float x_scale, float y_scale, float rotation, const float* color, int style);
	void R_AddCmdDrawStretchPic(game::qos::Material* material, float x, float y, float w, float h, float null1, float null2, float null3, float null4, float* color);
	void UI_DrawText(const game::qos::ScreenPlacement* ScrPlace, const char* text, int maxChars, game::qos::Font_s* font, float ix, float iy, int horzAlign, int vertAlign, float scale, const float* color, int style);


	unsigned int Scr_GetFunctionHandle(const char* filename, const char* funcHandle);
	void RemoveRefToObject(unsigned int obj);
	__int16 Scr_ExecThread(int handle);

	int Scr_LoadScript(const char* name);

}

#include "symbols.hpp"
