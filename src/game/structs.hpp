#pragma once

namespace game::qos
{
	typedef float vec_t;
	typedef vec_t vec3_t[3];
	typedef vec_t vec2_t[2];
	typedef vec_t vec4_t[4];
	typedef vec_t vec5_t[5];

	enum XAssetType
	{
		ASSET_TYPE_XMODELPIECES = 0x0,
		ASSET_TYPE_PHYSPRESET = 0x1,

		// these are assets from T4 & do not exist in IW4
		ASSET_TYPE_PHYSCONSTRAINTS = 0x2,
		ASSET_TYPE_DESTRUCTIBLE_DEF = 0x3,
		//

		ASSET_TYPE_XANIMPARTS = 0x4,
		ASSET_TYPE_XMODEL = 0x5,
		ASSET_TYPE_MATERIAL = 0x6,
		ASSET_TYPE_TECHNIQUE_SET = 0x7,
		ASSET_TYPE_IMAGE = 0x8,
		ASSET_TYPE_SOUND = 0x9,
		ASSET_TYPE_SOUND_CURVE = 0xA,
		//ASSET_TYPE_LOADED_SOUND = 11, // no loaded sounds?
		ASSET_TYPE_CLIPMAP_SP = 11,
		ASSET_TYPE_CLIPMAP_MP = 12, // ASSET_TYPE_CLIPMAP_PVS
		ASSET_TYPE_COMWORLD = 13,
		ASSET_TYPE_gameWORLD_SP = 14,
		ASSET_TYPE_gameWORLD_MP = 15,
		ASSET_TYPE_MAP_ENTS = 16,
		ASSET_TYPE_GFXWORLD = 17,
		ASSET_TYPE_LIGHT_DEF = 18,
		ASSET_TYPE_UI_MAP = 19, // i think?
		ASSET_TYPE_FONT = 20,
		ASSET_TYPE_MENULIST = 21,
		ASSET_TYPE_MENU = 22,
		ASSET_TYPE_LOCALIZE_ENTRY = 23,
		ASSET_TYPE_WEAPON = 24,
		ASSET_TYPE_SNDDRIVER_GLOBALS = 25, // i think?
		ASSET_TYPE_FX = 26,
		ASSET_TYPE_IMPACT_FX = 27,
		ASSET_TYPE_AITYPE = 28,
		ASSET_TYPE_MPTYPE = 29,
		ASSET_TYPE_CHARACTER = 30,
		ASSET_TYPE_XMODELALIAS = 31,
		ASSET_TYPE_RAWFILE = 32,
		ASSET_TYPE_STRINGTABLE = 33,

		// these assets only exist in QoS
		ASSET_TYPE_XML_TREE = 34,
		ASSET_TYPE_SCENE_ANIM_RESOURCE = 35,
		ASSET_TYPE_CUTSCENE_RESOURCE = 36,
		ASSET_TYPE_CUSTOM_CAMERA_LIST = 37,

		// just guessing here but double check later
		ASSET_TYPE_COUNT = 38,
		ASSET_TYPE_STRING = 38,
		ASSET_TYPE_ASSETLIST = 39,
		ASSET_TYPE_INVALID = -1
	};

	union XAssetHeader
	{
		void* data;
		//void* xmodelPieces;
		//PhysPreset* physPreset; // not done
		//void* physConstraints;
		//void* destructibleDef;
		//Material* material;
		//GfxImage* image;
		//XModel* xmodel;
		//ComWorld* comWorld;
		//gameWorldSp* gameWorldSp;
		//gameWorldMp* gameWorldMp;
		//clipMap_t* clipMap;
		//GfxWorld* gfxWorld;

		//RawFile* rawfile;
	};

	struct XAsset
	{
		XAssetType type;
		XAssetHeader header;
	};

	struct XAssetEntry
	{
		XAsset asset;
		char zoneIndex;
		char inuse;
		unsigned __int16 nextHash;
		unsigned __int16 nextOverride;
		unsigned __int16 usageFrame;
	};

	union XAssetEntryPoolEntry
	{
		XAssetEntry entry;
		XAssetEntryPoolEntry* next;
	};

	struct CmdArgs
	{
		int nesting;
		int localClientNum[8];
		int controllerIndex[8];
		int argc[8];
		const char** argv[8];
	};

	struct cmd_function_s
	{
		cmd_function_s* next;
		const char* name;
		const char* autoCompleteDir;
		const char* autoCompleteExt;
		void(__cdecl* function)();
	};

	struct scrMemTreePub_t
	{
		char* mt_buffer;
	};

	enum DvarType
	{
		DVAR_TYPE_BOOL = 0x0,
		DVAR_TYPE_FLOAT = 0x1,
		DVAR_TYPE_FLOAT_2 = 0x2,
		DVAR_TYPE_FLOAT_3 = 0x3,
		DVAR_TYPE_FLOAT_4 = 0x4,
		DVAR_TYPE_INT = 0x5,
		DVAR_TYPE_ENUM = 0x6,
		DVAR_TYPE_STRING = 0x7,
		DVAR_TYPE_COLOR = 0x8,
		DVAR_TYPE_COUNT = 0x9,
	};

	enum class dvar_type : std::int8_t
	{
		boolean = 0,
		value = 1,
		vec2 = 2,
		vec3 = 3,
		vec4 = 4,
		integer = 5,
		enumeration = 6,
		string = 7,
		color = 8,
		rgb = 9 // Color without alpha
	};

	struct DvarValueStringBuf
	{
		const char* pad;
		char string[12];
	};

	union DvarValue
	{
		bool enabled;
		int integer;
		unsigned int unsignedInt;
		float value;
		float vector[4];
		const char* string;
		DvarValueStringBuf stringBuf;
		char color[4];
	};

	union DvarLimits
	{
		struct enumeration
		{
			int stringCount;
			const char** strings;
		};
		struct integer
		{
			int min;
			int max;
		};
		struct value
		{
			float min;
			float max;
		};
		struct vector
		{
			float min;
			float max;
		};
	};

	enum dvar_flags : std::uint16_t
	{
		none = 0x0,
		saved = 0x1,
		user_info = 0x2, // sent to server on connect or change
		server_info = 0x4, // sent in response to front end requests
		replicated = 0x8,
		write_protected = 0x10,
		latched = 0x20,
		read_only = 0x40,
		cheat_protected = 0x80,
		temp = 0x100,
		no_restart = 0x400, // do not clear when a cvar_restart is issued
		user_created = 0x4000, // created by a set command
	};

	struct dvar_s
	{
		const char* name;
		const char* description;
		dvar_flags flags;
		dvar_type type;
		bool modified;
		DvarValue current;
		DvarValue latched;
		DvarValue reset;
		DvarLimits domain;
		dvar_s* next;
		dvar_s* hashNext;
	};

	typedef int scr_entref_t;

	typedef void(__stdcall* function_t)();

	struct scr_function_t
	{
		const char* name;
		function_t call;
		int developer;
	};

	typedef void(__stdcall* method_t)(scr_entref_t);

	struct scr_method_t
	{
		const char* name;
		method_t call;
		int developer;
	};

	union VariableUnion
	{
		int intValue;
		float floatValue;
		unsigned int stringValue;
		const float* vectorValue;
		const char* codePosValue;
		unsigned int pointerValue;
		struct VariableStackBuffer* stackValue;
		unsigned int entityOffset;
	};

	typedef struct
	{
		union VariableUnion u;
		int type;
	} VariableValue;

	struct function_stack_t
	{
		const char* pos;
		unsigned int localId;
		unsigned int localVarCount;
		VariableValue* top;
		VariableValue* startTop;
	};

	struct function_frame_t
	{
		function_stack_t fs;
		int topType;
	};

	struct scrVmPub_t
	{
		unsigned int* localVars;
		VariableValue* maxstack;
		int function_count;
		function_frame_t* function_frame;
		VariableValue* top;
		unsigned int inparamcount;
		unsigned int outparamcount;
		function_frame_t function_frame_start[32];
		VariableValue stack[2048];
	};

	enum VariableType
	{
		VAR_UNDEFINED = 0x0,
		VAR_BEGIN_REF = 0x1,
		VAR_POINTER = 0x1,
		VAR_STRING = 0x2,
		VAR_ISTRING = 0x3,
		VAR_VECTOR = 0x4,
		VAR_END_REF = 0x5,
		VAR_FLOAT = 0x5,
		VAR_INTEGER = 0x6,
		VAR_CODEPOS = 0x7,
		VAR_PRECODEPOS = 0x8,
		VAR_FUNCTION = 0x9,
		VAR_BUILTIN_FUNCTION = 0xA,
		VAR_BUILTIN_METHOD = 0xB,
		VAR_STACK = 0xC,
		VAR_ANIMATION = 0xD,
		VAR_DEVELOPER_CODEPOS = 0xE,
		VAR_INCLUDE_CODEPOS = 0xF,
		VAR_THREAD_LIST = 0x10,
		VAR_THREAD = 0x11,
		VAR_NOTIFY_THREAD = 0x12,
		VAR_TIME_THREAD = 0x13,
		VAR_CHILD_THREAD = 0x14,
		VAR_OBJECT = 0x15,
		VAR_DEAD_ENTITY = 0x16,
		VAR_ENTITY = 0x17,
		VAR_ARRAY = 0x18,
		VAR_DEAD_THREAD = 0x19,
		VAR_COUNT = 0x1A,
		VAR_FREE = 0x1A,
		VAR_ENDON_LIST = 0x1B,
	};

	struct Material
	{
		const char* name;
	};

	struct Font_s
	{
		const char* fontName;
		int pixelHeight;
		//int glyphCount;
		//Material* material;
		//Material* glowMaterial;
		//Glyph* glyphs;

		//havent mapped struct so idk if whats above matches
	};

	struct ScreenPlacement
	{
		float scaleVirtualToReal[2];
		float scaleVirtualToFull[2];
		float scaleRealToVirtual[2];
		float virtualScreenOffsetX;
		float virtualViewableMin[2];
		float virtualViewableMax[2];
		float realViewportSize[2];
		float realViewableMin[2];
		float realViewableMax[2];
		float subScreenLeft;
	}; //taken from Xbox, should match? I hope so...

}
