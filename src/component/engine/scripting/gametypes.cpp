#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "filesystem.hpp"
#include "gametypes.hpp"
#include "respawn_script.hpp"
#include "systemlink_progression.hpp"
#include "ambient_script.hpp"
#include "gametype_labels.hpp"
#include "scaleform.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/memory.hpp>
#include <utils/string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gametypes
{
	namespace
	{
		utils::hook::detour db_find_xasset_header_internal_hook;
		utils::hook::detour ui_load_gametypes_hook;
#if defined(DEBUG) || defined(_DEBUG)
		constexpr bool debug_modes = true;
#else
		constexpr bool debug_modes = false;
#endif
		std::uintptr_t custom_label_base, extra_label_base;
		std::uintptr_t custom_label_return, extra_label_return;

		const char* __cdecl localize_gametype_label(const char* reference)
		{
			if (!reference) return "";
			static thread_local std::string result;
			result = labels::resolve(reference, [](const char* key)
			{
				// QoS SEH_StringEd_GetString, confirmed cdecl 103C9F20;
				// uses the loaded language, not hard-coded English fallbacks.
				return reinterpret_cast<const char*(__cdecl*)(const char*)>(
					game::game_offset(0x103C9F20))(key);
			});
			return result.c_str();
		}

		__declspec(naked) void stock_label_entry()
		{
			__asm {
				push eax
				call localize_gametype_label
				add esp, 4
				ret
			}
		}
		__declspec(naked) void custom_label_entry()
		{
			__asm {
				add eax, custom_label_base
				push eax
				call localize_gametype_label
				add esp, 4
				jmp custom_label_return
			}
		}
		__declspec(naked) void extra_label_entry()
		{
			__asm {
				add eax, extra_label_base
				push eax
				call localize_gametype_label
				add esp, 4
				jmp extra_label_return
			}
		}

		void install_gametype_label_localization()
		{
			// QoS UI feeder 29, column 0: retail tail-calls EAX-based
			// UI_SafeTranslateString; custom/title-storage and appended rows
			// return label buffers directly. KisakCOD UI_GetGameTypeDisplayName
			// translates names too. Patch ONLY labels, never column-1 mode IDs.
			const auto stock = game::game_offset(0x102DE315);
			const auto custom = game::game_offset(0x102DE343);
			const auto extra = game::game_offset(0x102DE37B);
			unsigned char custom_bytes[] = {0x05, 0, 0, 0, 0};
			unsigned char extra_bytes[] = {0x05, 0, 0, 0, 0};
			custom_label_base = game::game_offset(0x113C6FA8);
			extra_label_base = game::game_offset(0x113D1C18);
			std::memcpy(custom_bytes + 1, &custom_label_base, 4);
			std::memcpy(extra_bytes + 1, &extra_label_base, 4);
			if (std::memcmp(reinterpret_cast<const void*>(stock), "\xE9\x96\xC6\xFF\xFF", 5)
				|| std::memcmp(reinterpret_cast<const void*>(custom), custom_bytes, 5)
				|| std::memcmp(reinterpret_cast<const void*>(extra), extra_bytes, 5))
			{
				console::error("gametypes: label localization skipped: unsupported feeder instructions\n");
				return;
			}
			custom_label_return = game::game_offset(0x102DE348);
			extra_label_return = game::game_offset(0x102DE380);
			utils::hook::jump(stock, stock_label_entry);
			utils::hook::jump(custom, custom_label_entry);
			utils::hook::jump(extra, extra_label_entry);
		}

		void fix_missing_asset_lock_release()
		{
			// QoS PC DB_FindXAssetHeaderInternal takes the write lock at
			// 103E2123, but its missing/create_default=0 branch skips unlock.
			// Redirect only that JZ to the existing unlock-and-null epilogue.
			// KisakCOD db_registry.cpp likewise unlocks before a missing return.
			// Retire this compatibility patch if the native lookup is replaced.
			const auto site = game::game_offset(0x103E21F3);
			const auto cleanup = game::game_offset(0x103E21DC);
			const unsigned char expected[] = {0x0F, 0x84, 0x2E, 0xFD, 0xFF, 0xFF};
			unsigned char cleanup_bytes[] = {
				0x68, 0, 0, 0, 0, 0xE8, 0x7A, 0xC7, 0xD2, 0xFF,
				0x33, 0xC0, 0x5F, 0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC3};
			const auto lock = static_cast<std::uint32_t>(game::game_offset(0x105624F8));
			std::memcpy(cleanup_bytes + 1, &lock, sizeof(lock));
			if (std::memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0
				|| std::memcmp(reinterpret_cast<const void*>(cleanup), cleanup_bytes, sizeof(cleanup_bytes)) != 0)
			{
				console::error("[assets] missing-asset lock fix skipped: unexpected native instructions\n");
				return;
			}

			utils::hook::set<std::int32_t>(site + 2, static_cast<std::int32_t>(cleanup - (site + 6)));
		}

		constexpr auto GAMETYPES_LIST = "maps/mp/gametypes/_gametypes.txt";
		constexpr auto GAMETYPE_PREFIX = "maps/mp/gametypes/";
		constexpr auto GAMETYPE_ENTRY_SIZE = 0x2C;
		constexpr auto UI_GAMETYPE_MAX = 0x20;
		constexpr auto UI_GAMETYPE_COUNT = 0x113D1684;
		constexpr auto UI_GAMETYPE_ENTRIES = 0x113D1688;
		constexpr auto UI_GAMETYPE_ALT_COUNT = 0x113D31EC;
		constexpr auto UI_GAMETYPE_ALT_ENTRIES = 0x113D31F0;

		int __cdecl ui_load_gametypes_stub()
		{
			const auto result = ui_load_gametypes_hook.invoke<int>();
			// QoS 102DB6F0 reloads both fixed 44-byte row arrays during UI init.
			// Compact complete rows so feeder indices still select matching IDs;
			// the secondary list's initial "All" row must remain untouched.
			for (const auto addresses : {std::pair{UI_GAMETYPE_COUNT, UI_GAMETYPE_ENTRIES},
				std::pair{UI_GAMETYPE_ALT_COUNT, UI_GAMETYPE_ALT_ENTRIES}})
			{
				auto* count = reinterpret_cast<int*>(game::game_offset(addresses.first));
				auto* rows = reinterpret_cast<char*>(game::game_offset(addresses.second));
				*count = labels::filter_modes(rows, *count, UI_GAMETYPE_MAX, debug_modes);
			}
			return result;
		}

		std::unordered_map<std::string, game::RawFile*>& loaded_gametype_rawfiles()
		{
			// pre_destroy runs during CRT exit, so the cache must outlive the
			// component registry that clears it.
			static auto* cache = new std::unordered_map<std::string, game::RawFile*>;
			return *cache;
		}
		bool ui_gametype_list_refreshed = false;

		const char* allocate_menu_string(const std::string& value)
		{
			auto* result = static_cast<char*>(utils::memory::allocate(value.size() + 1));
			std::memcpy(result, value.data(), value.size());
			result[value.size()] = '\0';
			return result;
		}

		// QoS 0x102DD390 loads this frontend list after ui_mp/code.txt.
		// Do not append to every list: the native registrar does not deduplicate.
		std::string normalize_gametype_path(const char* name)
		{
			std::string normalized = name ? name : "";
			for (auto& ch : normalized)
			{
				if (ch == '\\')
				{
					ch = '/';
				}
			}

			return normalized;
		}

		std::string trim(std::string value)
		{
			const auto first = value.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
			{
				return {};
			}

			const auto last = value.find_last_not_of(" \t\r\n");
			return value.substr(first, last - first + 1);
		}

		void copy_ui_gametype_string(char* dest, const std::size_t dest_size, const std::string& value)
		{
			if (dest == nullptr || dest_size == 0)
			{
				return;
			}

			strncpy_s(dest, dest_size, value.c_str(), _TRUNCATE);
		}

		std::string bounded_string(const char* value, const std::size_t max_length)
		{
			if (!value || max_length == 0)
			{
				return {};
			}

			std::string result{};
			result.reserve(max_length);
			for (std::size_t i = 0; i < max_length && value[i] != '\0'; ++i)
			{
				const auto ch = static_cast<unsigned char>(value[i]);
				result.push_back(ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.');
			}

			return result;
		}

		bool is_gametype_rawfile(const game::XAssetType type, const char* name)
		{
			const auto normalized = normalize_gametype_path(name);
			return type == game::ASSET_TYPE_RAWFILE
				&& !normalized.empty()
				&& _strnicmp(normalized.c_str(), GAMETYPE_PREFIX, std::strlen(GAMETYPE_PREFIX)) == 0;
		}

		bool rawfile_has_data(const game::RawFile* rawfile)
		{
			return rawfile != nullptr && rawfile->buffer != nullptr && rawfile->len > 0;
		}

		const char* get_zone_name(const unsigned char zone_index)
		{
			const auto* const zones = reinterpret_cast<game::XZone*>(game::game_offset(0x10AB8188));
			const auto* const zone = &zones[zone_index];
			return zone->name[0] ? zone->name : "";
		}

		bool is_patch_zone(const char* zone_name)
		{
			return zone_name != nullptr && _strnicmp(zone_name, "patch_", 6) == 0;
		}

		game::RawFile* make_rawfile(const std::string& name, const std::string& data)
		{
			auto* rawfile = utils::memory::allocate<game::RawFile>();
			auto* rawfile_name = static_cast<char*>(utils::memory::allocate(name.size() + 1));
			auto* buffer = static_cast<char*>(utils::memory::allocate(data.size() + 1));

			std::memcpy(rawfile_name, name.data(), name.size());
			std::memcpy(buffer, data.data(), data.size());
			rawfile_name[name.size()] = '\0';
			buffer[data.size()] = '\0';

			rawfile->name = rawfile_name;
			rawfile->len = static_cast<unsigned int>(data.size() + 1);
			rawfile->buffer = buffer;
			return rawfile;
		}

		game::RawFile* load_custom_gametype_rawfile(const char* name)
		{
			const auto normalized_name = normalize_gametype_path(name);
			if (const auto existing = loaded_gametype_rawfiles().find(normalized_name); existing != loaded_gametype_rawfiles().end())
			{
				return existing->second;
			}

			std::string data{};
			std::string real_path{};
			if (!filesystem::read_file(normalized_name, &data, &real_path))
			{
				return nullptr;
			}

			auto* rawfile = make_rawfile(normalized_name, data);
			loaded_gametype_rawfiles()[normalized_name] = rawfile;

			console::info("gametypes: loaded raw fallback %s from %s\n", normalized_name.c_str(), real_path.c_str());
			return rawfile;
		}

		game::XAssetHeader db_find_xasset_header_internal_stub(const game::XAssetType type, const char* name, const int create_default)
		{
			// QoS PC DB_FindXAssetHeaderInternal (103E1EE0) acquires the
			// database lock at 105624F8; its storage at +16 is released during
			// teardown. Calling it afterwards reaches a null CMPXCHG8B at
			// 1010E71D. No assets can be safely resolved without that lock.
			if (!*reinterpret_cast<void* volatile*>(game::game_offset(0x10562508)))
			{
				return {};
			}

			// Custom .menu loading/opening is handled entirely by menu_file's
			// direct hook on UI_OpenMenu_f (verified against the real native
			// menu-registration/open-stack layout, see menu_file.cpp). The
			// ASSET_TYPE_MENULIST/ASSET_TYPE_MENU augmentation that used to
			// live here went through an unverified capacity-check address
			// and silently no-op'd. Removed to avoid two competing
			// registration mechanisms.

			if (type == game::ASSET_TYPE_RAWFILE)
			{
				const auto path = normalize_gametype_path(name);
				if (path.starts_with("maps/mp/") && path.ends_with("_fx.gsc"))
				{
					auto map = path.substr(8, path.size() - 8 - 7);
					std::transform(map.begin(), map.end(), map.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
					if (ambient_script::stock_map(map) && !filesystem::exists(path))
					{
						auto original = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, name, create_default);
						const auto placement_path = "maps/createfx/" + map + "_fx.gsc";
						const auto placements = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(
							type, placement_path.c_str(), 0);
						// Namespaced patch inputs survive later map-zone asset links.
						// Keep the runtime sound-alias check: missing map sounds must
						// never become unconditional GSC playLoopSound calls.
						const auto bundled_source = "consolation/scripts/" + path;
						const auto bundled_placements = "consolation/scripts/" + placement_path;
						const auto patch_source = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, bundled_source.c_str(), 0);
						const auto patch_placements = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, bundled_placements.c_str(), 0);
						if (rawfile_has_data(patch_source.rawfile)) original = patch_source;
						const auto authored_file = rawfile_has_data(patch_placements.rawfile) ? patch_placements.rawfile : placements.rawfile;
						if (rawfile_has_data(original.rawfile) && rawfile_has_data(authored_file)
							&& !filesystem::exists(placement_path))
						{
							std::string data(original.rawfile->buffer, original.rawfile->len);
							while (!data.empty() && data.back() == '\0') data.pop_back();
							const std::string authored(authored_file->buffer, authored_file->len);
							if (ambient_script::restore_loops(data, authored, map, [](const std::string& alias)
							{
								const auto sound = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(
									game::ASSET_TYPE_SOUND, alias.c_str(), 0);
								return sound.data && !game::DB_IsXAssetDefault(game::ASSET_TYPE_SOUND, alias.c_str());
							}))
							{
								const auto key = "ambient:" + path + ":" + data;
								auto& cache = loaded_gametype_rawfiles();
								if (!cache.contains(key))
								{
									cache[key] = make_rawfile(path, data);
									console::info("[audio] enabled authored ambience fallback for %s\n", map.c_str());
								}
								original.rawfile = cache.at(key);
							}
						}
						return original;
					}
				}
				// QoS UI_LoadArenasFromFile 102CB9C0 requests this single DB
				// rawfile, not COD4's loose-file enumeration. Preserve stock maps
				// and append the installed DLC list; the native parser owns the
				// resulting map/gametype table and its 64-entry capacity checks.
				if (name && !_stricmp(name, "mp/jbmaps.arena"))
				{
					auto original = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, name, create_default);
					std::string extra;
					if (filesystem::read_file("mp/csl_dlc01.arena", &extra) && !extra.empty())
					{
						const auto* base = original.rawfile;
						if (rawfile_has_data(base) && extra.size() <= 8192 && extra.find('\0') == std::string::npos)
						{
							std::string data(base->buffer, base->len);
							while (!data.empty() && data.back() == '\0') data.pop_back();
							data += "\n" + extra;
							// Cache by full content, not just filename: UI may reload
							// after another base zone is linked. Allocations retain
							// native pointer lifetime like the existing rawfile cache.
							const auto key = std::string("arena:") + data;
							auto& cache = loaded_gametype_rawfiles();
							if (!cache.contains(key)) cache[key] = make_rawfile(name, data);
							original.rawfile = cache.at(key);
						}
					}
					return original;
				}
				// Use bundled movies once common_consolation is resident. Bootstrap
				// movies requested before that zone loads retain the loose fallback.
				if (const auto bundled = scaleform::bundled_path(name); !bundled.empty())
				{
					auto header = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(
						type, bundled.c_str(), 0);
					if (rawfile_has_data(header.rawfile)) return header;
				}
				if (auto* rawfile = scaleform::try_override(name))
				{
					game::XAssetHeader header{};
					header.rawfile = rawfile;
					return header;
				}
			}

			if (is_gametype_rawfile(type, name))
			{
				const auto normalized_name = normalize_gametype_path(name);
				if (auto* rawfile = load_custom_gametype_rawfile(name))
				{
					// Explicit disk overrides keep their author's policies.
					game::XAssetHeader header{};
					header.rawfile = rawfile;
					return header;
				}

				// Explicit disk overrides above remain authoritative. Otherwise
				// prefer pre-adapted scripts from patch_consolation; retaining the
				// old transforms below supports installs without the optional zone.
				if (normalized_name == "maps/mp/gametypes/_tweakables.gsc"
					|| normalized_name == "maps/mp/gametypes/_globallogic.gsc"
					|| normalized_name == "maps/mp/gametypes/_killcam.gsc"
					|| normalized_name == "maps/mp/gametypes/_damagefeedback.gsc"
					|| normalized_name == "maps/mp/gametypes/_persistence_util.gsc")
				{
					const auto bundled = "consolation/scripts/" + normalized_name;
					const auto patch = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, bundled.c_str(), 0);
					if (rawfile_has_data(patch.rawfile))
					{
						if (normalized_name == "maps/mp/gametypes/_tweakables.gsc")
						{
							std::string data(patch.rawfile->buffer, patch.rawfile->len);
							while (!data.empty() && data.back() == '\0') data.pop_back();
							if (respawn_script::migrate_legacy_respawn_default(data))
							{
								const auto key = std::string("respawn-default:") + data;
								auto& cache = loaded_gametype_rawfiles();
								if (!cache.contains(key)) cache[key] = make_rawfile(normalized_name.c_str(), data);
								game::XAssetHeader adapted{};
								adapted.rawfile = cache[key];
								return adapted;
							}
						}
						return patch;
					}
				}
				auto fastfile_header = db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, normalized_name.c_str(), create_default);
				if (rawfile_has_data(fastfile_header.rawfile))
				{
					if (normalized_name == "maps/mp/gametypes/_tweakables.gsc"
						|| normalized_name == "maps/mp/gametypes/_globallogic.gsc"
						|| normalized_name == "maps/mp/gametypes/_killcam.gsc"
						|| normalized_name == "maps/mp/gametypes/_damagefeedback.gsc"
						|| normalized_name == "maps/mp/gametypes/_persistence_util.gsc")
					{
						std::string data(fastfile_header.rawfile->buffer, fastfile_header.rawfile->len);
						while (!data.empty() && data.back() == '\0') data.pop_back();
						bool patched = normalized_name == "maps/mp/gametypes/_persistence_util.gsc"
							? systemlink_progression::enable_money(data)
							: normalized_name == "maps/mp/gametypes/_damagefeedback.gsc"
							? respawn_script::make_damage_feedback_visible(data)
							: normalized_name == "maps/mp/gametypes/_tweakables.gsc"
							? respawn_script::make_delay_live(data)
							: normalized_name == "maps/mp/gametypes/_killcam.gsc"
								? respawn_script::make_killcam_live(data) : respawn_script::make_wait_live(data);
						if (normalized_name == "maps/mp/gametypes/_globallogic.gsc")
						{
							// Independent adaptations: unsupported respawn anchors must
							// not prevent the verified match-clock visibility fix.
							const bool clock_patched = respawn_script::make_match_clock_hud(data);
							patched = patched || clock_patched;
						}
						if (patched)
						{
							const auto key = std::string("live-respawn:") + data;
							auto& cache = loaded_gametype_rawfiles();
							if (!cache.contains(key)) cache[key] = make_rawfile(normalized_name.c_str(), data);
							fastfile_header.rawfile = cache.at(key);
						}
						else if (data.find("// Consolation: live respawn delay") == std::string::npos
							&& data.find("consolationWaitRespawnDelay") == std::string::npos
							&& data.find("consolationKillcamPolicy") == std::string::npos
							&& data.find("// Consolation: local System Link money") == std::string::npos)
						{
							console::error("gametypes: live gameplay policy skipped: unsupported script %s\n", normalized_name.c_str());
						}
					}
					return fastfile_header;
				}
			}


			return db_find_xasset_header_internal_hook.invoke<game::XAssetHeader>(type, name, create_default);
		}

		std::string get_rawfile_buffer(const game::RawFile* rawfile)
		{
			if (!rawfile || !rawfile->buffer)
			{
				return {};
			}

			if (rawfile->len > 0)
			{
				const auto size = rawfile->buffer[rawfile->len - 1] == '\0'
					? rawfile->len - 1
					: rawfile->len;
				return { rawfile->buffer, rawfile->buffer + size };
			}

			return rawfile->buffer;
		}

		std::string read_gametype_rawfile(const std::string& name)
		{
			const auto header = game::DB_FindXAssetHeader_Internal(game::ASSET_TYPE_RAWFILE, name.c_str(), 1);
			return get_rawfile_buffer(header.rawfile);
		}

		std::vector<std::string> parse_gametype_ids(const std::string& data)
		{
			std::vector<std::string> ids{};
			std::istringstream stream(data);
			std::string line{};

			while (std::getline(stream, line) && ids.size() < UI_GAMETYPE_MAX)
			{
				if (const auto comment = line.find("//"); comment != std::string::npos)
				{
					line.erase(comment);
				}

				line = trim(line);
				if (!line.empty())
				{
					ids.push_back(line);
				}
			}

			return ids;
		}

		bool is_hidden_gametype_id(const std::string& gametype_id)
		{
			return labels::hidden_mode(gametype_id, debug_modes);
		}

		void append_unique_gametype_id(std::vector<std::string>& ids, std::unordered_set<std::string>& known_ids, const std::string& gametype_id)
		{
			auto normalized_id = trim(gametype_id);
			if (normalized_id.empty() || is_hidden_gametype_id(normalized_id) || ids.size() >= UI_GAMETYPE_MAX)
			{
				return;
			}

			std::transform(normalized_id.begin(), normalized_id.end(), normalized_id.begin(), [](const unsigned char ch)
			{
				return static_cast<char>(std::tolower(ch));
			});

			if (known_ids.insert(normalized_id).second)
			{
				ids.push_back(std::move(normalized_id));
			}
		}

		std::string get_gametype_id_from_display_path(const std::string& path)
		{
			auto normalized = normalize_gametype_path(path.c_str());
			if (_strnicmp(normalized.c_str(), GAMETYPE_PREFIX, std::strlen(GAMETYPE_PREFIX)) != 0)
			{
				const auto marker = normalized.find(GAMETYPE_PREFIX);
				if (marker == std::string::npos)
				{
					return {};
				}

				normalized = normalized.substr(marker);
			}

			if (!utils::string::ends_with(normalized, ".txt") || _stricmp(normalized.c_str(), GAMETYPES_LIST) == 0)
			{
				return {};
			}

			const auto slash = normalized.find_last_of('/');
			const auto dot = normalized.find_last_of('.');
			if (slash == std::string::npos || dot == std::string::npos || dot <= slash + 1)
			{
				return {};
			}

			return normalized.substr(slash + 1, dot - slash - 1);
		}

		void append_patch_gametype_ids(std::vector<std::string>& ids, std::unordered_set<std::string>& known_ids)
		{
			game::DB_EnumXAssetEntries(game::ASSET_TYPE_RAWFILE, [&](game::XAssetEntryPoolEntry* pool_entry)
			{
				if (ids.size() >= UI_GAMETYPE_MAX || pool_entry == nullptr)
				{
					return;
				}

				const auto& entry = pool_entry->entry;
				const auto* const rawfile = entry.asset.header.rawfile;
				if (!rawfile_has_data(rawfile) || !rawfile->name)
				{
					return;
				}

				const auto* const zone_name = get_zone_name(static_cast<unsigned char>(entry.zoneIndex));
				if (!is_patch_zone(zone_name))
				{
					return;
				}

				append_unique_gametype_id(ids, known_ids, get_gametype_id_from_display_path(rawfile->name));
			}, true);
		}

		void append_disk_gametype_ids(std::vector<std::string>& ids, std::unordered_set<std::string>& known_ids)
		{
			for (const auto& search_path : filesystem::get_search_paths())
			{
				if (ids.size() >= UI_GAMETYPE_MAX)
				{
					break;
				}

				const auto directory = std::filesystem::path(search_path) / "maps/mp/gametypes";
				if (!utils::io::directory_exists(directory.generic_string()))
				{
					continue;
				}

				for (const auto& file : utils::io::list_files(directory.generic_string()))
				{
					if (ids.size() >= UI_GAMETYPE_MAX)
					{
						break;
					}

					append_unique_gametype_id(ids, known_ids, get_gametype_id_from_display_path(file));
				}
			}
		}

		std::vector<std::string> get_visible_gametype_ids(const std::string& gametype_list)
		{
			std::vector<std::string> ids{};
			std::unordered_set<std::string> known_ids{};

			for (const auto& gametype_id : parse_gametype_ids(gametype_list))
			{
				append_unique_gametype_id(ids, known_ids, gametype_id);
			}

			append_patch_gametype_ids(ids, known_ids);
			append_disk_gametype_ids(ids, known_ids);
			return ids;
		}

		std::string parse_gametype_display_name(const std::string& data, const std::string& fallback)
		{
			return labels::display_token(data, fallback);
		}

		void rebuild_ui_gametype_cache()
		{
			const auto gametype_list = read_gametype_rawfile(GAMETYPES_LIST);
			if (gametype_list.empty())
			{
				console::warn("gametypes: cannot refresh UI list because %s was empty or unavailable\n", GAMETYPES_LIST);
				return;
			}

			const auto gametype_ids = get_visible_gametype_ids(gametype_list);
			if (gametype_ids.empty())
			{
				console::warn("gametypes: cannot refresh UI list because %s parsed no entries\n", GAMETYPES_LIST);
				return;
			}

			auto* const count = reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_COUNT));
			auto* const entries = reinterpret_cast<char*>(game::game_offset(UI_GAMETYPE_ENTRIES));
			auto* const alt_count = reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_ALT_COUNT));
			auto* const alt_entries = reinterpret_cast<char*>(game::game_offset(UI_GAMETYPE_ALT_ENTRIES));

			std::memset(entries, 0, GAMETYPE_ENTRY_SIZE * UI_GAMETYPE_MAX);
			std::memset(alt_entries, 0, GAMETYPE_ENTRY_SIZE * UI_GAMETYPE_MAX);

			auto written_count = 0;
			auto visible_count = 0;
			std::vector<std::string> hidden_gametype_ids{};
			for (const auto& gametype_id : gametype_ids)
			{
				const auto display_file = std::format("{}{}.txt", GAMETYPE_PREFIX, gametype_id);
				const auto display_data = read_gametype_rawfile(display_file);
				if (display_data.empty())
				{
					hidden_gametype_ids.push_back(gametype_id);
					continue;
				}

				const auto display_name = parse_gametype_display_name(display_data, gametype_id);
				auto* const entry = entries + (written_count * GAMETYPE_ENTRY_SIZE);
				auto* const alt_entry = alt_entries + (written_count * GAMETYPE_ENTRY_SIZE);

				copy_ui_gametype_string(entry, 12, gametype_id);
				copy_ui_gametype_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, display_name);
				copy_ui_gametype_string(alt_entry, 12, gametype_id);
				copy_ui_gametype_string(alt_entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, display_name);
				++written_count;
				++visible_count;
			}

			for (const auto& gametype_id : hidden_gametype_ids)
			{
				if (written_count >= UI_GAMETYPE_MAX)
				{
					break;
				}

				auto* const entry = entries + (written_count * GAMETYPE_ENTRY_SIZE);
				auto* const alt_entry = alt_entries + (written_count * GAMETYPE_ENTRY_SIZE);

				copy_ui_gametype_string(entry, 12, gametype_id);
				copy_ui_gametype_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, gametype_id);
				copy_ui_gametype_string(alt_entry, 12, gametype_id);
				copy_ui_gametype_string(alt_entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C, gametype_id);
				++written_count;
			}

			*count = written_count;
			*alt_count = written_count;
			ui_gametype_list_refreshed = true;
			console::info("gametypes: refreshed UI gametype lists with %i visible entries, %i total entries\n", visible_count, written_count);
		}

		void dump_loaded_gametypes_rawfile()
		{
			std::string data{};
			std::string source{};
			if (!filesystem::read_file(GAMETYPES_LIST, &data, &source))
			{
				const auto asset = game::DB_FindXAssetHeader_Internal(game::ASSET_TYPE_RAWFILE, GAMETYPES_LIST, 1);
				data = get_rawfile_buffer(asset.rawfile);
				source = "fastfile";
			}

			if (data.empty())
			{
				console::warn("dumpGametypes: %s was empty or unavailable\n", GAMETYPES_LIST);
				return;
			}

			const auto out_path = "consolation/gametypes_dump/_gametypes.txt";
			if (utils::io::write_file(out_path, data))
			{
				console::info("dumpGametypes: dumped %s from %s to %s\n", GAMETYPES_LIST, source.c_str(), out_path);
			}
			else
			{
				console::error("dumpGametypes: failed to write %s\n", out_path);
			}
		}

		void dump_parsed_ui_gametypes()
		{
			const auto count = std::clamp(*reinterpret_cast<int*>(game::game_offset(0x113D1684)), 0, UI_GAMETYPE_MAX);
			const auto* const entries = reinterpret_cast<const char*>(game::game_offset(0x113D1688));

			std::string output{};
			for (auto i = 0; i < count; ++i)
			{
				const auto* const entry = entries + (i * GAMETYPE_ENTRY_SIZE);
				output.append(std::format(
					"{} \"{}\"\r\n",
					bounded_string(entry, 12),
					bounded_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C)));
			}

			const auto out_path = "consolation/gametypes_dump/parsed_ui_gametypes.txt";
			if (utils::io::write_file(out_path, output))
			{
				console::info("dumpGametypes: dumped %i parsed UI gametype(s) to %s\n", count, out_path);
			}
			else
			{
				console::error("dumpGametypes: failed to write %s\n", out_path);
			}
		}

		bool safe_read_byte(const std::uintptr_t address, unsigned char* value)
		{
			if (!value)
			{
				return false;
			}

			SIZE_T bytes_read = 0;
			return ReadProcessMemory(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(address),
				value,
				sizeof(*value),
				&bytes_read) && bytes_read == sizeof(*value);
		}

		std::string get_exe_directory()
		{
			char path[MAX_PATH]{};
			const auto length = GetModuleFileNameA(nullptr, path, sizeof(path));
			if (length == 0 || length >= sizeof(path))
			{
				return {};
			}

			std::string result = path;
			const auto separator = result.find_last_of("/\\");
			if (separator == std::string::npos)
			{
				return {};
			}

			result.resize(separator);
			return result;
		}

		std::string resolve_game_path(const std::string& path)
		{
			if (path.size() > 2 && path[1] == ':')
			{
				return path;
			}

			const auto base = get_exe_directory();
			if (base.empty())
			{
				return path;
			}

			return base + "\\" + path;
		}

		bool create_directory_tree(const std::string& path, DWORD* error)
		{
			if (path.empty())
			{
				return true;
			}

			auto create_one = [error](const std::string& directory)
			{
				if (directory.empty() || (directory.size() == 2 && directory[1] == ':'))
				{
					return true;
				}

				if (CreateDirectoryA(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
				{
					return true;
				}

				if (error)
				{
					*error = GetLastError();
				}

				return false;
			};

			for (std::size_t i = 0; i < path.size(); ++i)
			{
				if (path[i] != '/' && path[i] != '\\')
				{
					continue;
				}

				if (!create_one(path.substr(0, i)))
				{
					return false;
				}
			}

			return create_one(path);
		}

		bool write_file_noexcept(const std::string& path, const std::string& data, DWORD* error)
		{
			const auto resolved_path = resolve_game_path(path);
			const auto separator = resolved_path.find_last_of("/\\");
			if (separator != std::string::npos && !create_directory_tree(resolved_path.substr(0, separator), error))
			{
				return false;
			}

			const auto file = CreateFileA(resolved_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				if (error)
				{
					*error = GetLastError();
				}

				return false;
			}

			DWORD written = 0;
			const auto size = static_cast<DWORD>(std::min<std::size_t>(data.size(), MAXDWORD));
			const auto ok = WriteFile(file, data.data(), size, &written, nullptr) && written == size;
			if (!ok && error)
			{
				*error = GetLastError();
			}

			CloseHandle(file);
			return ok;
		}

		void append_bytes(std::string& output, const char* label, const std::uintptr_t address, const std::size_t length)
		{
			output.append(std::format("  {} @ {:08X}: ", label, address));
			for (std::size_t i = 0; i < length; ++i)
			{
				unsigned char byte = 0;
				if (safe_read_byte(address + i, &byte))
				{
					output.append(std::format("{:02X} ", byte));
				}
				else
				{
					output.append("?? ");
				}
			}

			output.append("\r\n");
		}

		void register_commands()
		{
			command::add("dumpGametypes", [](const command::params&)
			{
				dump_loaded_gametypes_rawfile();
				dump_parsed_ui_gametypes();
			});
		}

		class component final : public component_interface
		{
		public:
			void post_load() override
			{
				fix_missing_asset_lock_release();
				install_gametype_label_localization();
				db_find_xasset_header_internal_hook.create(game::DB_FindXAssetHeader_Internal, db_find_xasset_header_internal_stub);
				// Verified no-argument, caller-clean native loader. Retire this
				// compatibility hook when the native frontend cache is replaced.
				const auto loader = game::game_offset(0x102DB6F0);
				if (!std::memcmp(reinterpret_cast<const void*>(loader), "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x0C", 9))
					ui_load_gametypes_hook.create(loader, ui_load_gametypes_stub);
				else console::error("gametypes: MENU visibility hook skipped: unsupported loader instructions\n");

				// The frontend feeder count path subtracts one to hide the stock "menu" sentinel.
				// MENU is filtered by ID in Release, retained in Debug: expose
				// the full filtered count rather than dropping an arbitrary last row.
				utils::hook::nop(game::game_offset(0x102DEA4A), 3);

				// The gametype feeder row resolver has its own hide-last subtraction before
				// returning row text/id. Keep it in sync with the count path above.
				utils::hook::nop(game::game_offset(0x102DE2F5), 3);

				// Native feeder selection has a third hide-last subtraction.
				// Keep selection bounds identical to count/text (including Gun Game).
				const auto selection_count = game::game_offset(0x102DC9CF);
				if (!std::memcmp(reinterpret_cast<const void*>(selection_count), "\x83\xC5\xFF", 3))
					utils::hook::nop(selection_count, 3);
				else console::error("gametypes: selection bounds fix skipped: unsupported instructions\n");

				// Scaleform requests a fixed-size row window. Ignore that window limit and let the
				// feeder submit every available item up to the real total count.
				utils::hook::nop(game::game_offset(0x102D4A12), 10);

				// Keep command registration in command.cpp if this dump helper is needed later.
			}

				void pre_destroy() override
				{
					db_find_xasset_header_internal_hook.clear();
					ui_load_gametypes_hook.clear();
					loaded_gametype_rawfiles().clear();
					scaleform::clear_overrides();
				ui_gametype_list_refreshed = false;
			}
		};
	}

	void refresh_ui_gametype_list()
	{
		if (ui_gametype_list_refreshed)
		{
			return;
		}

		rebuild_ui_gametype_cache();
	}

	void force_refresh_ui_gametype_list()
	{
		rebuild_ui_gametype_cache();
	}

	void dump_debug_state()
	{
		const auto count = std::clamp(*reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_COUNT)), 0, UI_GAMETYPE_MAX);
		const auto alt_count = std::clamp(*reinterpret_cast<int*>(game::game_offset(UI_GAMETYPE_ALT_COUNT)), 0, UI_GAMETYPE_MAX);
		const auto* const entries = reinterpret_cast<const char*>(game::game_offset(UI_GAMETYPE_ENTRIES));
		const auto* const alt_entries = reinterpret_cast<const char*>(game::game_offset(UI_GAMETYPE_ALT_ENTRIES));

		std::string output{};
		output.append(std::format("primary count: {} @ {:08X}\r\n", count, game::game_offset(UI_GAMETYPE_COUNT)));
		output.append(std::format("primary entries: {:08X}\r\n", game::game_offset(UI_GAMETYPE_ENTRIES)));
		for (auto i = 0; i < count; ++i)
		{
			const auto* const entry = entries + (i * GAMETYPE_ENTRY_SIZE);
			output.append(std::format(
				"  [{}] id='{}' display='{}'\r\n",
				i,
				bounded_string(entry, 12),
				bounded_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C)));
		}

		output.append(std::format("\r\nsecondary count: {} @ {:08X}\r\n", alt_count, game::game_offset(UI_GAMETYPE_ALT_COUNT)));
		output.append(std::format("secondary entries: {:08X}\r\n", game::game_offset(UI_GAMETYPE_ALT_ENTRIES)));
		for (auto i = 0; i < alt_count; ++i)
		{
			const auto* const entry = alt_entries + (i * GAMETYPE_ENTRY_SIZE);
			output.append(std::format(
				"  [{}] id='{}' display='{}'\r\n",
				i,
				bounded_string(entry, 12),
				bounded_string(entry + 0x0C, GAMETYPE_ENTRY_SIZE - 0x0C)));
		}

		output.append("\r\nknown feeder patches:\r\n");
		output.append(std::format("  count hide-last instruction: {:08X}\r\n", game::game_offset(0x102DEA4A)));
		output.append(std::format("  gametype row resolver: {:08X}\r\n", game::game_offset(0x102DE2EF)));
		output.append(std::format("  gametype row hide-last instruction: {:08X}\r\n", game::game_offset(0x102DE2F5)));
		output.append(std::format("  feeder draw switch case 271: {:08X}\r\n", game::game_offset(0x102DC430)));
		append_bytes(output, "count hide-last bytes", game::game_offset(0x102DEA4A), 3);
		append_bytes(output, "row hide-last bytes", game::game_offset(0x102DE2F5), 3);
		append_bytes(output, "scaleform window-limit bytes", game::game_offset(0x102D4A12), 10);

		output.append("\r\nnote: ASSET_TYPE_MENU is the IW .menu layer, not Scaleform .gfx, so this dump intentionally avoids enumerating it.\r\n");

		const auto out_path = "consolation/gametypes_dump/debug_state.txt";
		DWORD error = ERROR_SUCCESS;
		if (write_file_noexcept(out_path, output, &error))
		{
			console::info("dumpGametypesDebug: wrote %s\n", out_path);
		}
		else
		{
			console::error("dumpGametypesDebug: failed to write %s (GetLastError=%lu)\n", out_path, error);
		}
	}
}

REGISTER_COMPONENT(gametypes::component)
