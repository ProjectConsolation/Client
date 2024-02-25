#include <std_include.hpp>

#include "component/console.hpp"

#include "game/game.hpp"
#include <utils/string.hpp>
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

	// half of this is inlined on QoS, so just re-writing it all since its little work
	qos::cmd_function_s* Cmd_FindCommand(const char* name)
	{
		qos::cmd_function_s* command;

		for (command = *cmd_functions; command; command = command->next)
		{
			if (!strcmp(name, command->name))
			{
				return command;
			}
		}

		return 0;
	}

	void Cmd_AddCommandInternal(const char* name, void(__cdecl* function)(), qos::cmd_function_s* cmd)
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

	const char* DB_GetXAssetName(game::qos::XAsset* asset)
	{
		if (!asset) return "";

		assert(asset->header.data);

		return game::DB_XAssetGetNameHandler[asset->type](&asset->header);
	}

	bool DB_IsXAssetDefault(qos::XAssetType type, const char* name)
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

	void DB_EnumXAssetEntries(qos::XAssetType type, std::function<void(qos::XAssetEntryPoolEntry*)> callback, bool overrides)
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
					qos::XAssetEntryPoolEntry* asset = &g_assetEntryPool[hashIndex];
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

	bool Key_IsCatcherActive(int mask)
	{
		return (mask & game::clientUI->keyCatchers);
	}

	// end of IW3SP

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

	//IW3SP-MOD's code, thanks :)
	void ScrPlace_ApplyRect(const game::qos::ScreenPlacement* ScrPlace, float* x, float* y, float* w, float* h, int horzAlign, int vertAlign)
	{
		float v7;
		float v8;
		float v9;
		float v10;

		switch (horzAlign)
		{
		case 7:
			v7 = *x * ScrPlace->scaleVirtualToReal[0];
			v8 = (float)(ScrPlace->realViewableMin[0] + ScrPlace->realViewableMax[0]) * 0.5;
			*x = v7 + v8;
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		case 6:
			*x = *x * ScrPlace->scaleRealToVirtual[0];
			*w = *w * ScrPlace->scaleRealToVirtual[0];
			break;
		case 5:
			break;
		case 4:
			*x = *x * ScrPlace->scaleVirtualToFull[0];
			*w = *w * ScrPlace->scaleVirtualToFull[0];
			break;
		case 3:
			*x = (float)(*x * ScrPlace->scaleVirtualToReal[0]) + ScrPlace->realViewableMax[0];
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		case 2:
			v7 = *x * ScrPlace->scaleVirtualToReal[0];
			v8 = 0.5 * ScrPlace->realViewportSize[0];
			*x = v7 + v8;
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		case 1:
			*x = (float)(*x * ScrPlace->scaleVirtualToReal[0]) + ScrPlace->realViewableMin[0];
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		default:
			*x = (float)(*x * ScrPlace->scaleVirtualToReal[0]) + ScrPlace->subScreenLeft;
			*w = *w * ScrPlace->scaleVirtualToReal[0];
			break;
		}

		switch (vertAlign)
		{
		case 7:
			v9 = *y * ScrPlace->scaleVirtualToReal[1];
			v10 = (float)(ScrPlace->realViewableMin[1] + ScrPlace->realViewableMax[1]) * 0.5;
			*y = v9 + v10;
			*h = *h * ScrPlace->scaleVirtualToReal[1];
		case 6:
			*y = *y * ScrPlace->scaleRealToVirtual[1];
			*h = *h * ScrPlace->scaleRealToVirtual[1];
			break;
		case 5:
			return;
		case 4:
			*y = *y * ScrPlace->scaleVirtualToFull[1];
			*h = *h * ScrPlace->scaleVirtualToFull[1];
			break;
		case 3:
			*y = (float)(*y * ScrPlace->scaleVirtualToReal[1]) + ScrPlace->realViewableMax[1];
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		case 2:
			v9 = *y * ScrPlace->scaleVirtualToReal[1];
			v10 = 0.5 * ScrPlace->realViewportSize[1];
			*y = v9 + v10;
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		case 1:
			*y = (float)(*y * ScrPlace->scaleVirtualToReal[1]) + ScrPlace->realViewableMin[1];
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		default:
			*y = *y * ScrPlace->scaleVirtualToReal[1];
			*h = *h * ScrPlace->scaleVirtualToReal[1];
			break;
		}
	}

	//xoxor3d's code, thanks :)
	void R_AddCmdDrawTextASM(const char* text, int max_chars, void* font, float x, float y, float x_scale, float y_scale, float rotation, const float* color, int style)
	{
		const static uint32_t R_AddCmdDrawText_func = game::game_offset(0x103C02B0); //AddBaseDrawTextCmd, no idea if it works :shrug:
		__asm
		{
			push	style;
			sub     esp, 14h;

			fld		rotation;
			fstp[esp + 10h];

			fld		y_scale;
			fstp[esp + 0Ch];

			fld		x_scale;
			fstp[esp + 8];

			fld		y;
			fstp[esp + 4];

			fld		x;
			fstp[esp];

			push	font;
			push	max_chars;
			push	text;
			mov		ecx, [color];

			call	R_AddCmdDrawText_func;
			add		esp, 24h;
		}
	}

	void R_AddCmdDrawStretchPic(game::qos::Material* material, float x, float y, float w, float h, float null1, float null2, float null3, float null4, float* color)
	{
		const static uint32_t R_AddCmdDrawStretchPic_func = game::game_offset(0x103C0820);
		__asm
		{
			pushad;
			push	color;
			mov		eax, [material];
			sub		esp, 20h;

			fld		null4;
			fstp[esp + 1Ch];

			fld		null3;
			fstp[esp + 18h];

			fld		null2;
			fstp[esp + 14h];

			fld		null1;
			fstp[esp + 10h];

			fld		h;
			fstp[esp + 0Ch];

			fld		w;
			fstp[esp + 8h];

			fld		y;
			fstp[esp + 4h];

			fld		x;
			fstp[esp];

			call	R_AddCmdDrawStretchPic_func;
			add		esp, 24h;
			popad;
		}
	}

	//IW3SP-MOD's code, thanks :)
	void UI_DrawText(const game::qos::ScreenPlacement* ScrPlace, const char* text, int maxChars, game::qos::Font_s* font, float ix, float iy, int horzAlign, int vertAlign, float scale, const float* color, int style)
	{
		float xScale = scale * 48.0f / static_cast<float>(font->pixelHeight);
		float yScale = xScale;

		ScrPlace_ApplyRect(ScrPlace, &ix, &iy, &xScale, &yScale, horzAlign, vertAlign);
		int x = floor(ix + 0.5);
		int y = floor(iy + 0.5);
		R_AddCmdDrawTextASM(text, maxChars, font, x, y, xScale, yScale, 0.0, color, style);
	}

	// IW3SP-MOD, thanks again, though this one was simple, lul
	game::qos::ScreenPlacement* ScrPlace_GetFullPlacement()
	{
		return game::scrPlaceFull;
	}

	const char* drawVersion()
	{
		//auto* const scrPlace = ScrPlace_GetFullPlacement();

		//const float color[4] = { 0.0f, 0.80f, 0.0f, 0.69f };
		//game::qos::Font_s* fontHandle = game::R_RegisterFont("fonts/objectiveFont", sizeof("fonts/objectiveFont"));

		//float fontScale = (0.2f * 48.0f / static_cast<float>(fontHandle->pixelHeight)) * game::scrPlace->scaleVirtualToReal[0];
		//float xPos = 50.f * game::scrPlace->scaleVirtualToReal[0];
		//float yPos = 18.f * game::scrPlace->scaleVirtualToReal[1];
		//UI_DrawText(scrPlace, utils::string::va("%d %s %s", 549, "CONSOLATION", buffer), 128, fontHandle, xPos, yPos, 0, 0, fontScale, color, 0);
		const char* out = utils::string::va("%d %s %s", 549, "CONSOLATION", utils::string::get_timestamp);
		return out;
	}
}
