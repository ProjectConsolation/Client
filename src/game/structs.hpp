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

	enum class dvar_type : std::int8_t
	{ //IW3SP
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

	enum dvar_flags : std::uint16_t
	{ //IW3SP
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
		saved_flag = 0x1000,
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

}
