#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "engine/console/command.hpp"
#include "engine/console/console.hpp"
#include "utils/scheduler.hpp"
#include "utils/hook.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"
#include "engine/console/ip_endpoint.hpp"
#include "engine/patches/xlive.hpp"
#include "engine/scripting/menu_file.hpp"
#include "steam_identity.hpp"
#include "steam_companion.hpp"
#include "steam_transport.hpp"
#include "steam_friends.hpp"
#include "engine/scripting/gametype_labels.hpp"
#include <charconv>
#include <memory>

#ifdef CSL_STEAMWORKS
#pragma warning(push)
#pragma warning(disable: 4996) // Vendor SDK uses strncpy in inline helpers.
#pragma warning(disable: 4828) // SDK 1.65 has ANSI copyright bytes in three headers.
#include <steam/steam_api.h>
#pragma warning(pop)
#endif

// Default Steam control plane with nonfatal offline fallback. It does not replace QoS sessions, XUIDs,
// persistence. Steam Networking Messages carries unchanged QoS datagrams through
// a loopback-only UDP bridge; connectIP retains the native query/handshake. The
// offline XUID/profile contract remains separate from authenticated Steam peers.
namespace steam_matchmaking
{
	namespace
	{
		std::string vote_display_name(const std::string& id, const bool map)
		{
			const char* reference = id.c_str();
			if (map)
			{
				// QoS UI arena table: 128 records, longname[32], map[32] (102DB2B0).
				const auto count = *reinterpret_cast<const int*>(game::game_offset(0x113D3770));
				if (count > 0 && count <= 128)
					for (int i = 0; i < count; ++i)
					{
						const auto* entry = reinterpret_cast<const char*>(game::game_offset(0x113D3774)) + i * 64;
						if (strnlen(entry, 32) < 32 && strnlen(entry + 32, 32) < 32 && !_stricmp(entry + 32, id.c_str()))
						{ reference = entry; break; }
					}
			}
			else reference = reinterpret_cast<const char*(__cdecl*)(const char*)>(game::game_offset(0x102D9A20))(id.c_str());
			return gametypes::labels::resolve(reference ? reference : id, [](const char* key) {
				return reinterpret_cast<const char*(__cdecl*)(const char*)>(game::game_offset(0x103C9F20))(key);
			});
		}
		bool browser_show_listen()
		{
			const auto* setting = game::Dvar_FindVar("ui_browserShowNonDedicated");
			if (!setting) return true;
			if (static_cast<unsigned char>(setting->type) == game::DVAR_TYPE_STRING)
				return setting->current.string && (!std::strcmp(setting->current.string, "1") || !_stricmp(setting->current.string, "true"));
			if (static_cast<unsigned char>(setting->type) == game::DVAR_TYPE_BOOL) return setting->current.enabled;
			return setting->current.integer != 0;
		}
		utils::hook::detour select_playlist_hook, leave_match_hook, party_go_hook;
		utils::hook::detour friends_menu_hook;
		std::function<void()> friends_menu_action;
		void friends_menu_stub() { if (friends_menu_action) friends_menu_action(); }
		std::function<void()> find_match_action, cancel_match_action;
		std::function<bool()> party_go_action;

		unsigned int party_go_stub()
		{
			if (party_go_action && party_go_action()) return 0;
			return party_go_hook.invoke<unsigned int>();
		}

		void find_match_stub()
		{
			if (find_match_action) find_match_action();
		}

		void leave_match_stub()
		{
			if (cancel_match_action) cancel_match_action();
			leave_match_hook.invoke<void>(); // retain native party cleanup
		}

		void find_match_error(const char* message)
		{
			console::warn("Steam Find Match: %s\n", message);
			// QoS 1.1 selectplaylist's error path: title in EDI, return screen
			// and message on the stack, caller cleanup. Copies all three strings.
			const auto entry = game::game_offset(0x1040E7F0);
			constexpr unsigned char expected[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56};
			if (std::memcmp(reinterpret_cast<const void*>(entry), expected, sizeof(expected))) return;
			const char* title = "@MPUI_ERROR_TITLE";
			const char* screen = "mpxblivemain";
			__asm
			{
				push edi
				mov edi, title
				push message
				push screen
				call entry
				add esp, 8
				pop edi
			}
		}
	}
#ifdef CSL_STEAMWORKS
	namespace
	{
		constexpr auto product = "project-consolation";
		constexpr auto protocol = "qos-pc-1.1-lobby-v3-auth";
		bool find_match_server_kind(std::string_view kind) { return kind == "listen"; }
		bool browser_server_kind(std::string_view kind) { return kind == "listen" || kind == "dedicated"; }

		// QoS 1.1 10309770 arms pregame + visible game-start time, cancels
		// when minimum membership/editing gates fail, and 103252E0 sets the
		// minimum lobby time on awake. Steam owns the roster here; never fake
		// a native party structure to call those LIVE-dependent routines.
		struct lobby_countdown
		{
			unsigned long long opened = 0, deadline = 0;
			int pregame = -1, duration = -1, minimum_time = -1;
			void reset() { *this = {}; }
			bool advance(unsigned long long now, bool allowed, int pre, int game, int lobby_time)
			{
				if (!allowed) { deadline = 0; return false; }
				if (pregame != pre || duration != game || minimum_time != lobby_time)
				{
					deadline = 0; pregame = pre; duration = game; minimum_time = lobby_time;
				}
				if (now < opened + static_cast<unsigned long long>(lobby_time) * 1000) return false;
				if (!deadline) deadline = now + static_cast<unsigned long long>(pre + game) * 1000;
				return now >= deadline;
			}
			int seconds(unsigned long long now) const
			{
				return deadline > now ? static_cast<int>((deadline - now + 999) / 1000) : 0;
			}
			bool visible(unsigned long long now) const { return deadline && seconds(now) <= duration; }
		};

		// QoS PC parser 102E1D60: map[32], gametype[16], weight at +48,
		// 56-byte entries at playlist +1344, count at +8512, stride 8532.
		// Read-only views; names/layout come from QoS, not another title.
		struct rotation_entry
		{
			char map[32];
			char gametype[16];
			int weight;
			int remaining;
		};
		struct playlist_view
		{
			char name[64], description[256], rules[1024];
			rotation_entry entries[128];
			int count;
			char tail[16];
		};
		static_assert(sizeof(rotation_entry) == 56);
		static_assert(offsetof(rotation_entry, weight) == 48);
		static_assert(offsetof(playlist_view, entries) == 1344);
		static_assert(offsetof(playlist_view, count) == 8512);
		static_assert(sizeof(playlist_view) == 8532);

		bool matches_rotation(const std::vector<rotation_entry>& rotation, const char* map, const char* mode)
		{
			for (const auto& entry : rotation)
				if (entry.weight > 0 && std::memchr(entry.map, 0, sizeof(entry.map))
					&& std::memchr(entry.gametype, 0, sizeof(entry.gametype))
					&& !std::strcmp(entry.map, map) && !std::strcmp(entry.gametype, mode)) return true;
			return false;
		}

		bool prefer_host(bool running, std::uint64_t owner, std::uint64_t self,
			bool best_running, std::uint64_t best)
		{
			return owner && owner != self && (running || owner < self)
				&& ((!best_running && running) || (running == best_running && owner < best));
		}

		bool copy_rotation(const playlist_view* playlists, int ordinal,
			std::vector<rotation_entry>& rotation, std::string& name)
		{
			if (!playlists || ordinal < 0 || ordinal >= 32) return false;
			// Native 102E1A20 maps the displayed ordinal to nonempty table IDs.
			for (int id = 0, index = 0; id < 32; ++id)
			{
				const auto& playlist = playlists[id];
				if (!playlist.count) continue;
				if (playlist.count < 0 || playlist.count > 128) return false;
				if (index++ != ordinal) continue;
				if (!std::memchr(playlist.name, 0, sizeof(playlist.name))) return false;
				rotation.assign(playlist.entries, playlist.entries + playlist.count);
				name = playlist.name;
				return true;
			}
			return false;
		}

		bool selected_rotation(std::vector<rotation_entry>& rotation, std::string& name)
		{
			if (!*reinterpret_cast<const unsigned char*>(game::game_offset(0x115FB9A8))) return false;
			const auto* selected = game::Dvar_FindVar("playlist");
			if (!selected) return false;
			int ordinal = selected->current.integer;
			const auto type = static_cast<unsigned char>(selected->type);
			if (type != game::DVAR_TYPE_INT && type != game::DVAR_TYPE_ENUM)
			{
				if (type != game::DVAR_TYPE_STRING || !selected->current.string) return false;
				const std::string_view text = selected->current.string;
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ordinal);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
			}
			return copy_rotation(reinterpret_cast<const playlist_view*>(game::game_offset(0x11384508)), ordinal, rotation, name);
		}

		bool safe_host_token(const char* text, std::size_t capacity)
		{
			const auto* end = static_cast<const char*>(std::memchr(text, 0, capacity));
			if (!end || end == text) return false;
			for (auto* p = text; p != end; ++p)
				if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z')
					&& !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-') return false;
			return true;
		}

		int choose_host_entry()
		{
			// QoS 102E1C80: previous entry in EAX, weighted quota selection in
			// the native table, result EAX. -1 excludes no previous entry.
			const auto entry = game::game_offset(0x102E1C80);
			constexpr unsigned char expected[] = {0x51, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF8};
			if (std::memcmp(reinterpret_cast<const void*>(entry), expected, sizeof(expected))) return -1;
			int selected = -1;
			__asm
			{
				mov eax, -1
				call entry
				mov selected, eax
			}
			return selected;
		}

		bool apply_host_rules(int selected)
		{
			// QoS 102E2AA0: entry index in EAX, controller on stack. It applies
			// the selected playlist/gametype rules and populates ui_mapname and
			// ui_gametype, consumed by native UI StartServer (102E08C0).
			const auto entry = game::game_offset(0x102E2AA0);
			constexpr unsigned char expected[] = {0x53, 0x55, 0x56, 0x57, 0x8B, 0xF8};
			if (std::memcmp(reinterpret_cast<const void*>(entry), expected, sizeof(expected))) return false;
			__asm
			{
				push 0
				mov eax, selected
				call entry
				add esp, 4
			}
			return true;
		}

		bool valid_game_ip(uint32 ip)
		{
			// Steam IPs are host-order. Never connect to unspecified, loopback,
			// multicast/broadcast or link-local addresses supplied by a lobby.
			return (ip >> 24) != 0 && (ip >> 24) != 127 && (ip >> 24) < 224
				&& (ip >> 16) != 0xA9FE;
		}

		[[maybe_unused]] bool parse_host_endpoint(const std::string& text, uint32& ip, uint16& port)
		{
			std::string_view address;
			unsigned int number = 0;
			IN_ADDR parsed{};
			if (!direct_connect::parse_endpoint(text, "", address, number)
				|| InetPtonA(AF_INET, std::string(address).c_str(), &parsed) != 1) return false;
			ip = ntohl(parsed.s_addr);
			port = static_cast<uint16>(number);
			return valid_game_ip(ip);
		}

		[[maybe_unused]] uint32 local_game_ip()
		{
			// No external IP service or arbitrary first-adapter choice. VPNs and
			// multihomed hosts must specify their reachable LAN address explicitly.
			char hostname[256]{};
			if (gethostname(hostname, sizeof(hostname))) return 0;
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			addrinfo* addresses = nullptr;
			if (getaddrinfo(hostname, nullptr, &hints, &addresses)) return 0;
			uint32 selected = 0;
			bool ambiguous = false;
			for (auto* entry = addresses; entry; entry = entry->ai_next)
			{
				const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr);
				if (!valid_game_ip(ip)) continue;
				if (selected && selected != ip) ambiguous = true;
				selected = ip;
			}
			freeaddrinfo(addresses);
			return ambiguous ? 0 : selected;
		}

		class service final
		{
		public:
			std::recursive_mutex mutex;

			void initialize()
			{
				if (stopped_ || active_) return;
				next_init_attempt_ = GetTickCount64() + 5000;
				// IW4x Proxy::SetGame also sets SteamAppId before initializing.
				// This adapter uses the supported SDK, not its internal client ABI.
				// Preserve an explicit Steam launch or local developer App ID.
				std::error_code appid_error;
				const bool has_appid_file = std::filesystem::exists("steam_appid.txt", appid_error);
				if (appid_error)
				{
					unavailable("Cannot inspect steam_appid.txt; not overriding the configured identity.");
					return;
				}
				if (!GetEnvironmentVariableA("SteamAppId", nullptr, 0) &&
					!GetEnvironmentVariableA("SteamGameId", nullptr, 0) && !has_appid_file)
				{
					const auto default_id = std::to_string(CSL_STEAM_APP_ID);
					if (!SetEnvironmentVariableA("SteamAppId", default_id.c_str()))
					{
						unavailable("Cannot set the default Steam App ID.");
						return;
					}
					console::info("Steam: using configured default App ID %s%s.\n", default_id.c_str(),
						CSL_STEAM_APP_ID == 480 ? " (Spacewar development test, not a release identity)" : "");
				}
				// Load only a redistributable explicitly installed beside the host.
				// Keep the module resident: delay imports and SDK callback destructors
				// can still reference it during CRT teardown.
				wchar_t host[MAX_PATH]{};
				const auto length = GetModuleFileNameW(nullptr, host, MAX_PATH);
				if (!length || length >= MAX_PATH)
				{
					unavailable("Cannot locate the Steam API redistributable.");
					return;
				}
				const auto dll = std::filesystem::path(host).parent_path() / L"steam_api.dll";
				std::error_code file_error;
				if (!module_ && std::filesystem::is_regular_file(dll, file_error))
					module_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				if (!module_)
				{
					unavailable("Install the SDK's x86 steam_api.dll beside the game executable.");
					return;
				}
				// The SDK can immediately send GamesPlayed. Install Companion's
				// hook first so its initial report is captured for registration resend.
				if (!presence_.prepare())
				{
					next_init_attempt_ = GetTickCount64() + 100;
					return;
				}
				SteamErrMsg error{};
				if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK)
				{
					unavailable(error);
					return;
				}
				active_ = true;
				if (!SteamUser() || !SteamUtils() || !SteamFriends() || !SteamMatchmaking())
				{
					SteamAPI_Shutdown();
					active_ = false;
					unavailable("Required SDK interfaces unavailable.");
					return;
				}
				invites_.Register(this, &service::invited);
				if (SteamNetworkingMessages() && SteamNetworkingUtils())
				{
					// Force SDR rather than disclosing LAN/public IPs through ICE.
					relay_enabled_ = steam_transport::bridge::secure_ingress(0) && SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, 0);
					if (!relay_enabled_) console::warn("Steam gameplay disabled: deploy the matching security-hardened xlive.dll and d3d9.dll.\n");
					SteamNetworkingUtils()->InitRelayNetworkAccess();
					relay_requests_.Register(this, &service::relay_requested);
					relay_failures_.Register(this, &service::relay_failed);
				}
				last_init_error_.clear();
				kicked_.Register(this, &service::kicked);
				game_created_.Register(this, &service::game_created);
				console::info("Steam initialized for App ID %u. Lobby title: Project: Consolation.\n", SteamUtils()->GetAppID());
				check_sign_in();
				presence_.initialize(SteamUtils()->GetAppID());
				update_identity();
				status();
			}

			void pump()
			{
				// Steam may not be ready on our first main frame. Keep offline
				// startup nonfatal, but recover without requiring a console command.
				if (!active_ && !stopped_ && GetTickCount64() >= next_init_attempt_)
					initialize();
				if (active_ && !stopped_)
				{
					SteamAPI_RunCallbacks();
					if (leave_requested_ && !pending_) leave();
					if (browser_refresh_queued_ && !pending_ && !finding_ && !resolving_find_host_)
					{
						browser_refresh_queued_ = false;
						browser_refresh();
					}
					game::Dvar_SetFromStringByName("csl_browser_available", relay_enabled_ && SteamUser()->BLoggedOn() ? "1" : "0");
					pump_transport();
					check_sign_in();
					if (finding_ && GetTickCount64() - find_started_ >= 20000)
						finish_find("Steam search timed out. Check your connection and try Find Match again.");
					if (GetTickCount64() - last_game_poll_ >= 1000)
					{
						last_game_poll_ = GetTickCount64();
						publish_roster();
						const std::string presence_lobby = SteamUser()->BLoggedOn() && lobby_.IsValid() ? std::to_string(lobby_.ConvertToUint64()) : "";
						if (presence_lobby != friends_presence_lobby_)
						{
							if (SteamFriends()->SetRichPresence("csl_lobby", presence_lobby.empty() ? nullptr : presence_lobby.c_str())) friends_presence_lobby_ = presence_lobby;
						}
						friends_refresh();
						persist_public_stats();
						maintain_auto_lobby();
						publish_game();
						connect_game();
						update_lobby_screen();
						update_identity();
						update_presence();
					}
				}
			}

			void status()
			{
				if (!active_) { console::info("Steam inactive; offline play unchanged. Use steam_init.\n"); return; }
				console::info("Steam: app=%u user=%llu online=%d lobby=%llu pending=%d\n",
					SteamUtils()->GetAppID(), SteamUser()->GetSteamID().ConvertToUint64(),
					SteamUser()->BLoggedOn(), lobby_.ConvertToUint64(), pending_);
				console::info("Steam gameplay transport: %s.\n", relay_enabled_ ? "Steam Networking Messages / SDR" : "unavailable; direct System Link remains available");
				transport_status();
				presence_.print_status();
				console::info("Steam Find Match: searching=%d waiting_host=%d playlist=%s\n", finding_, waiting_host_, find_name_.c_str());
				const auto* progress = game::Dvar_FindVar("csl_public_match");
				const auto* stats = reinterpret_cast<const unsigned char*>(game::game_offset(0x116101C8));
				console::info("Steam progression: public=%d fetched=%u dirty=%u (native mpdata; leaderboard upload is separate)\n",
					progress && progress->current.enabled, stats[0], stats[1]);
				if (lobby_.IsValid())
				{
					console::info("Steam lobby: %d/%d members.\n", SteamMatchmaking()->GetNumLobbyMembers(lobby_),
						SteamMatchmaking()->GetLobbyMemberLimit(lobby_));
					console::info("Steam lobby flow: state=%s minimum=%s timer=%s visible=%s autostart=%s\n",
						SteamMatchmaking()->GetLobbyData(lobby_, "csl_host_state"), SteamMatchmaking()->GetLobbyData(lobby_, "csl_minplayers"),
						SteamMatchmaking()->GetLobbyData(lobby_, "csl_timer"), SteamMatchmaking()->GetLobbyData(lobby_, "csl_timer_visible"),
						SteamMatchmaking()->GetLobbyData(lobby_, "csl_autostart"));
				}
			}

			void start_match()
			{
				if (!active_ || !SteamUser()->BLoggedOn() || pending_ || !waiting_host_ || running_server() || !lobby_.IsValid()
					|| SteamMatchmaking()->GetLobbyOwner(lobby_) != SteamUser()->GetSteamID())
				{ console::warn("Steam: only the waiting Find Match host can use steam_match_start.\n"); return; }
				const auto votes = vote_counts();
				if (votes[1] > votes[0]) host_entry_ = vote_entries_[1];
				else host_entry_ = vote_entries_[0];
				launch_find_host(true);
			}

			bool party_go()
			{
				// Keep original private/System Link behavior outside our playlist
				// lobby. Within it, start_match rechecks host ownership and loading.
				if (!active_ || !lobby_.IsValid() || !*SteamMatchmaking()->GetLobbyData(lobby_, "csl_vote_round")) return false;
				start_match();
				return true;
			}

			void set_name(const std::string& name)
			{
				manual_name_ = steam_identity::display_name(name);
				game::Dvar_SetFromStringByName("steam_useSteamName", "0");
				update_identity();
				console::info("Name preference: %s (an active match keeps its existing name until the lobby/next match).\n", manual_name_.c_str());
			}

			void presence_status() { presence_.print_status(); }
			void presence_retry() { presence_.retry(); }

			void vote(const int choice)
			{
				if (!active_ || !SteamUser()->BLoggedOn() || !lobby_.IsValid() || choice < 0 || choice > 1) return;
				auto* mm = SteamMatchmaking();
				if (std::strcmp(mm->GetLobbyData(lobby_, "csl_host_state"), "waiting")) return;
				const std::string round = mm->GetLobbyData(lobby_, "csl_vote_round");
				if (round.empty() || round.size() > 20 || round.find_first_not_of("0123456789") != std::string::npos) return;
				mm->SetLobbyMemberData(lobby_, "csl_vote", (round + ":" + std::to_string(choice)).c_str());
				update_lobby_screen();
			}

			void find_match()
			{
				if (!ready()) { find_match_error("Sign into Steam and wait for any pending lobby operation before searching."); return; }
				if (lobby_.IsValid() || running_server())
				{ find_match_error("Leave your Steam lobby and stop your own match before using Find Match."); return; }
				if (!xlive::is_local_shim_loaded())
				{ find_match_error("Install the matching Consolation LAN xlive.dll before using Steam Find Match."); return; }
				using party_count = int(__thiscall*)(void*);
				if (reinterpret_cast<party_count>(game::game_offset(0x10322720))(
					reinterpret_cast<void*>(game::game_offset(0x111CF170))) > 1)
				{ find_match_error("Steam Find Match currently supports solo joining, not moving a grouped party."); return; }
				if (!selected_rotation(find_rotation_, find_name_))
				{ find_match_error("The selected playlist is unavailable. Reload the playlists and try again."); return; }
				finding_ = true; find_join_pending_ = cancelled_find_join_ = false;
				find_started_ = GetTickCount64(); candidates_.clear();
				console::info("Steam Find Match: searching for running hosts in %s.\n", find_name_.c_str());
				list();
				if (!pending_) finish_find("Steam could not submit the lobby search. Please try again.");
			}

			void cancel_find(const bool include_waiting = true)
			{
				if (!include_waiting && lobby_.IsValid() && playlist_lobby_) return;
				if (!finding_ && (!include_waiting || !waiting_host_)) return;
				finding_ = false;
				waiting_host_ = false;
				candidates_.clear();
				// Keep an in-flight JoinLobby result registered so a late successful
				// join can be left immediately, never orphaning membership.
				if (find_host_pending_) cancelled_auto_create_ = true;
				else if (find_join_pending_) cancelled_find_join_ = true;
				else { listed_.Cancel(); pending_ = false; }
				if (auto_lobby_ && !auto_match_started_ && !host_launch_started_ && !pending_)
				{
					host_resolve_at_ = 0; resolving_find_host_ = false;
					leave(); // cancelled reservation, not a running hosted match
				}
				console::info("Steam Find Match: search ended/cancelled.\n");
			}

			void create(const bool public_lobby, const std::string& endpoint)
			{
				if (!ready() || lobby_.IsValid()) return;
				if (!relay_enabled_ || !xlive::is_local_shim_loaded())
				{ console::error("Steam hosting requires Steam relay support and Consolation's offline shim.\n"); return; }
				host_ip_ = 0; host_port_ = 0;
				if (!endpoint.empty()) console::warn("Steam: legacy host address ignored; gameplay uses SDR.\n");
				// No routable LAN address is required by the Steam transport.
				const auto call = SteamMatchmaking()->CreateLobby(public_lobby ? k_ELobbyTypePublic : k_ELobbyTypeFriendsOnly, 18);
				if (begin(call)) created_.Set(call, this, &service::created);
			}

			void list(const bool browser = false)
			{
				if (!ready()) return;
				auto* matchmaking = SteamMatchmaking();
				matchmaking->AddRequestLobbyListStringFilter("csl_product", product, k_ELobbyComparisonEqual);
				matchmaking->AddRequestLobbyListStringFilter("csl_protocol", protocol, k_ELobbyComparisonEqual);
				if (browser && !browser_show_listen())
					matchmaking->AddRequestLobbyListStringFilter("csl_server_kind", "dedicated", k_ELobbyComparisonEqual);
				if (!browser) matchmaking->AddRequestLobbyListFilterSlotsAvailable(1);
				matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
				matchmaking->AddRequestLobbyListResultCountFilter(50);
				const auto call = matchmaking->RequestLobbyList();
				if (begin(call)) listed_.Set(call, this, &service::listed);
			}

			void browser_refresh()
			{
				if (pending_ || finding_ || resolving_find_host_)
				{ console::warn("Server Browser: wait for the current Steam operation.\n"); return; }
				browser_rows_.clear();
				game::Dvar_SetFromStringByName("csl_browser_count", "0");
				game::Dvar_SetFromStringByName("csl_browser_revision", std::to_string(++browser_revision_).c_str());
				if (!ready()) return;
				game::Dvar_SetFromStringByName("csl_browser_available", "1");
				browser_pending_ = true;
				list(true);
				if (!pending_) browser_pending_ = false;
			}

			void browser_filter()
			{
				game::Dvar_SetFromStringByName("ui_browserShowNonDedicated", browser_show_listen() ? "0" : "1");
				if (pending_ || finding_ || resolving_find_host_) { browser_refresh_queued_ = true; return; }
				browser_refresh(); // retires the old row-to-lobby mapping/revision
			}

			void browser_join(const uint64 revision, const unsigned int index)
			{
				if (revision != browser_revision_ || browser_pending_ || index >= browser_rows_.size())
				{ console::warn("Server Browser: stale selection; refresh and select again.\n"); return; }
				const auto target = browser_rows_[index];
				if (!browser_eligible(target)) { console::warn("Server Browser: this host is no longer available.\n"); return; }
				if (SteamMatchmaking()->GetNumLobbyMembers(target) >= SteamMatchmaking()->GetLobbyMemberLimit(target))
				{ console::warn("Server Browser: this lobby is full.\n"); return; }
				join(std::to_string(target.ConvertToUint64()));
			}

			void join(const std::string& text)
			{
				if (!ready() || lobby_.IsValid()) return;
				uint64 id = 0;
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
				const CSteamID target(id);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !target.IsValid() || !target.IsLobby())
				{
					console::error("Usage: steam_lobby_join <decimal lobby SteamID>\n");
					return;
				}
				const auto call = SteamMatchmaking()->JoinLobby(target);
				if (begin(call))
				{
					find_join_pending_ = finding_;
					joined_.Set(call, this, &service::joined);
				}
			}

			void leave()
			{
				if (pending_)
				{
					leave_requested_ = true;
					menu_file::set_match_lobby(false);
					console::info("Steam: leaving as soon as the pending lobby operation completes.\n");
					return;
				}
				leave_requested_ = false;
				const auto* stats = reinterpret_cast<const unsigned char*>(game::game_offset(0x116101C8));
				if (playlist_lobby_ && stats[0] && stats[1]) command::execute("uploadStats");
				if (active_) SteamFriends()->SetRichPresence("csl_lobby", nullptr);
				friends_presence_lobby_.clear();
				close_transport();
				cancel_find();
				// Do not cancel an in-flight join/create: cancelling its call result
				// would hide a successful join and leak untracked lobby membership.
				if (active_ && lobby_.IsValid())
				{
					if (SteamMatchmaking()->GetLobbyOwner(lobby_) == SteamUser()->GetSteamID())
					{
						SteamMatchmaking()->SetLobbyData(lobby_, "csl_game_ready", "0");
						SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "closed");
					}
					SteamMatchmaking()->LeaveLobby(lobby_);
				}
				lobby_ = CSteamID();
				menu_file::set_match_lobby(false);
				auto_lobby_ = auto_match_started_ = false;
				playlist_lobby_ = false;
				waiting_host_ = false;
				host_launch_started_ = waiting_game_since_ = 0;
				game::Dvar_SetFromStringByName("csl_public_match", "0");
				countdown_.reset();
				host_resolve_at_ = 0; resolving_find_host_ = false;
				// Respect an explicit leave while the same server is running.
				suppress_auto_lobby_ = running_server();
				host_ip_ = published_ip_ = attempted_ip_ = 0;
				host_port_ = published_port_ = attempted_port_ = 0;
				published_ready_ = false;
				observed_round_.clear();
			}

			void friends_open(const bool scaleform = false)
			{
				if (scaleform) game::Dvar_SetFromStringByName("csl_friends_scaleform", "1");
				else if (!menu_file::show_friends(true)) { console::warn("Friends: open from a frontend/lobby, not during map loading or gameplay.\n"); return; }
				friends_snapshot_.page = 0;
				friends_snapshot_.scroll_to(0);
				friends_notice_.clear();
				if (active_ && SteamUser()->BLoggedOn())
					for (int i = 0; i < std::clamp(SteamFriends()->GetFriendCount(k_EFriendFlagImmediate), 0, 256); ++i)
						SteamFriends()->RequestFriendRichPresence(SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate));
				friends_refresh();
			}
			CSteamID friend_lobby(const CSteamID id) const
			{
				// Companion may change the displayed game identity. Use our explicit
				// app-scoped lobby presence first; it is only a join candidate, never
				// proof of product compatibility or authenticated native membership.
				const auto* text = SteamFriends()->GetFriendRichPresence(id, "csl_lobby");
				const auto length = text ? strnlen(text, 21) : 0;
				if (length && length <= 20)
				{
					uint64 value = 0;
					const auto parsed = std::from_chars(text, text + length, value);
					const CSteamID lobby(value);
					if (parsed.ec == std::errc{} && parsed.ptr == text + length && lobby.IsValid() && lobby.IsLobby()) return lobby;
				}
				FriendGameInfo_t game{};
				return SteamFriends()->GetFriendGamePlayed(id, &game) && game.m_gameID.AppID() == SteamUtils()->GetAppID()
					&& game.m_steamIDLobby.IsLobby() ? game.m_steamIDLobby : CSteamID{};
			}
			void friends_refresh()
			{
				if (!menu_file::friends_visible()) return;
				std::vector<steam_friends::entry> entries;
				if (active_ && SteamUser()->BLoggedOn())
				{
					auto* api = SteamFriends();
					const auto count = std::clamp(api->GetFriendCount(k_EFriendFlagImmediate), 0, 256);
					for (int i = 0; i < count; ++i)
					{
						const auto id = api->GetFriendByIndex(i, k_EFriendFlagImmediate);
						if (!id.IsValid() || !id.BIndividualAccount() || api->GetFriendRelationship(id) != k_EFriendRelationshipFriend) continue;
						steam_friends::entry entry; entry.id = id.ConvertToUint64();
						entry.name = steam_friends::label(api->GetFriendPersonaName(id));
						const auto state = api->GetFriendPersonaState(id);
						entry.online = state != k_EPersonaStateOffline;
						entry.status = !entry.online ? "Offline" : state == k_EPersonaStateBusy ? "Busy" :
							state == k_EPersonaStateAway || state == k_EPersonaStateSnooze ? "Away" : "Online";
						const auto lobby = friend_lobby(id);
						if (lobby.IsValid()) { entry.lobby = lobby.ConvertToUint64(); entry.status += " / In a lobby"; }
						entries.push_back(std::move(entry));
					}
				}
				friends_snapshot_.update(std::move(entries));
				menu_file::set_friends_item("status", !active_ || !SteamUser()->BLoggedOn() ? "Sign into Steam to view friends." :
					GetTickCount64() < friends_notice_until_ ? friends_notice_ :
					friends_snapshot_.entries.empty() ? "No Steam friends found." : std::to_string(friends_snapshot_.entries.size()) + " Steam friends - select a friend below");
				menu_file::set_friends_item("page", "PAGE " + std::to_string(friends_snapshot_.page + 1) + " / " + std::to_string(friends_snapshot_.pages()));
				game::Dvar_SetFromStringByName("csl_friends_count", std::to_string(friends_snapshot_.entries.size()).c_str());
				game::Dvar_SetFromStringByName("csl_friends_first", std::to_string(friends_snapshot_.first_visible).c_str());
				for (std::size_t row = 0; row < steam_friends::snapshot::page_size; ++row)
				{
					const auto index = friends_snapshot_.first_visible + row;
					const auto* entry = index < friends_snapshot_.entries.size() ? &friends_snapshot_.entries[index] : nullptr;
					game::Dvar_SetFromStringByName(("csl_friends_id" + std::to_string(row)).c_str(),
						entry ? std::to_string(entry->id).c_str() : "");
					menu_file::set_friends_item("friend" + std::to_string(row), entry ?
						std::string(entry->id == friends_snapshot_.selected ? "> " : "  ") + entry->name + "   -   " + entry->status : "",
						entry ? "exec \"steam_friend_select " + std::to_string(entry->id) + "\";" : "");
				}
				const auto* selected = friends_snapshot_.get(friends_snapshot_.selected);
				menu_file::set_friends_item("selected", selected ? "SELECTED: " + selected->name : "SELECT A FRIEND");
			}
			void friends_select(const uint64 id)
			{
				if (!menu_file::friends_visible() || !friends_snapshot_.get(id)) return;
				friends_snapshot_.selected = id;
				friends_refresh();
			}
			void friends_page(const bool next)
			{
				if (next && friends_snapshot_.page + 1 < friends_snapshot_.pages()) ++friends_snapshot_.page;
				else if (!next && friends_snapshot_.page) --friends_snapshot_.page;
				friends_snapshot_.scroll_to(friends_snapshot_.page * steam_friends::snapshot::page_size);
				friends_refresh();
			}
			void friends_scroll(const int first)
			{
				if (!menu_file::friends_visible()) return;
				friends_snapshot_.scroll_to(static_cast<std::size_t>(std::clamp(first, 0, 256)));
				friends_refresh();
			}
			void friends_action(const bool invite_friend)
			{
				if (!menu_file::friends_visible()) return;
				const CSteamID friend_id(friends_snapshot_.selected);
				const auto notice = [this](const char* text) { friends_notice_ = text; friends_notice_until_ = GetTickCount64() + 5000; menu_file::set_friends_item("status", text); };
				if (!active_ || !SteamUser()->BLoggedOn()) { notice("Sign into Steam first."); return; }
				if (!friend_id.IsValid() || SteamFriends()->GetFriendRelationship(friend_id) != k_EFriendRelationshipFriend)
				{ notice("Select a current Steam friend first."); return; }
				if (invite_friend)
				{
					if (!lobby_.IsValid() || pending_) { notice("Join or create a Steam lobby before inviting friends."); return; }
					// A successful request is not a delivery/join acknowledgement.
					notice(SteamMatchmaking()->InviteUserToLobby(lobby_, friend_id) ? "Steam invitation requested." : "Steam could not send the invitation.");
					return;
				}
				if (lobby_.IsValid() || running_server() || pending_)
				{ notice("Leave your current lobby/match before joining a friend."); return; }
				const auto target = friend_lobby(friend_id);
				if (!target.IsValid())
				{ notice("This friend has no joinable Steam lobby."); return; }
				// App ID 480 is not product proof; shared join()/joined() validate
				// product, protocol and security before any native connection.
				menu_file::show_friends(false);
				join(std::to_string(target.ConvertToUint64()));
			}
			void invite()
			{
				if (active_ && lobby_.IsValid()) SteamFriends()->ActivateGameOverlayInviteDialog(lobby_);
				else console::warn("Steam: create or join a lobby first.\n");
			}

			void shutdown()
			{
				game::Dvar_SetFromStringByName("csl_browser_available", "0");
				close_transport();
				command::set_steam_identity(0);
				presence_.shutdown();
				stopped_ = true;
				finding_ = false; candidates_.clear();
				created_.Cancel(); listed_.Cancel(); joined_.Cancel();
				if (active_)
				{
					SteamFriends()->SetRichPresence("csl_lobby", nullptr);
					relay_requests_.Unregister(); relay_failures_.Unregister();
					// SDK 1.65 CCallback::Unregister calls the DLL unconditionally,
					// even for an unregistered manual callback. steam.dmp captured
					// __delayLoadHelper2 from this path after missing-DLL startup.
					// Only initialized sessions have registered these callbacks.
					invites_.Unregister(); kicked_.Unregister(); game_created_.Unregister();
					if (lobby_.IsValid()) SteamMatchmaking()->LeaveLobby(lobby_);
					SteamAPI_Shutdown();
				}
				active_ = false; pending_ = false; lobby_ = CSteamID();
				menu_file::set_match_lobby(false);
			}

		private:
			bool active_ = false, pending_ = false, stopped_ = false;
			ULONGLONG next_init_attempt_ = 0;
			std::string last_init_error_;
			bool offline_warning_shown_ = false;
			ULONGLONG last_stats_save_ = 0;
			bool stats_was_ingame_ = false;
			bool relay_enabled_ = false;
			steam_transport::bridge transport_;
			steam_friends::snapshot friends_snapshot_;
			std::string friends_presence_lobby_, friends_notice_;
			ULONGLONG friends_notice_until_ = 0;
			std::string published_native_identity_;
			static constexpr int relay_channel = 17;
			steam_companion::client presence_;
			steam_identity::policy identity_;
			std::string manual_name_;
			bool identity_reserved_ = false;
			HMODULE module_ = nullptr;
			CSteamID lobby_;
			uint32 host_ip_ = 0, published_ip_ = 0, attempted_ip_ = 0;
			uint16 host_port_ = 0, published_port_ = 0, attempted_port_ = 0;
			bool published_ready_ = false;
			ULONGLONG last_game_poll_ = 0;
			bool finding_ = false, find_join_pending_ = false, cancelled_find_join_ = false;
			bool find_host_pending_ = false, cancelled_auto_create_ = false;
			bool auto_create_requested_ = false, auto_lobby_ = false, auto_match_started_ = false;
			bool suppress_auto_lobby_ = false;
			bool waiting_host_ = false;
			bool playlist_lobby_ = false;
			uint64 vote_round_ = 0;
			lobby_countdown countdown_;
			std::array<int, 2> vote_entries_{-1, -1};
			std::string observed_round_;
			ULONGLONG last_auto_attempt_ = 0, host_launch_started_ = 0;
			ULONGLONG waiting_game_since_ = 0;
			ULONGLONG host_resolve_at_ = 0;
			bool resolving_find_host_ = false;
			int host_entry_ = -1;
			ULONGLONG find_started_ = 0;
			std::string find_name_;
			std::vector<rotation_entry> find_rotation_;
			std::vector<CSteamID> candidates_;
			CCallResult<service, LobbyCreated_t> created_;
			CCallResult<service, LobbyMatchList_t> listed_;
			CCallResult<service, LobbyEnter_t> joined_;
			bool browser_pending_ = false;
			bool leave_requested_ = false;
			bool browser_refresh_queued_ = false;
			uint64 browser_revision_ = 0;
			std::vector<CSteamID> browser_rows_;
			CCallbackManual<service, GameLobbyJoinRequested_t> invites_;
			CCallbackManual<service, LobbyKicked_t> kicked_;
			CCallbackManual<service, LobbyGameCreated_t> game_created_;
			CCallbackManual<service, SteamNetworkingMessagesSessionRequest_t> relay_requests_;
			CCallbackManual<service, SteamNetworkingMessagesSessionFailed_t> relay_failures_;

			std::vector<std::uint64_t> relay_members() const
			{
				std::vector<std::uint64_t> members;
				if (!active_ || !lobby_.IsValid()) return members;
				auto* mm = SteamMatchmaking();
				const auto self = SteamUser()->GetSteamID();
				for (int i = 0; i < std::min(mm->GetNumLobbyMembers(lobby_), 18); ++i)
				{
					const auto peer = mm->GetLobbyMemberByIndex(lobby_, i);
					if (peer.IsValid() && peer.BIndividualAccount() && peer != self) members.push_back(peer.ConvertToUint64());
				}
				return members;
			}

			void close_transport()
			{
				if (active_ && relay_enabled_ && SteamNetworkingMessages()) for (const auto id : relay_members())
				{ SteamNetworkingIdentity peer{}; peer.SetSteamID64(id); SteamNetworkingMessages()->CloseSessionWithUser(peer); }
				transport_.shutdown();
				published_native_identity_.clear();
			}

			void relay_requested(SteamNetworkingMessagesSessionRequest_t* request)
			{
				const auto members = relay_members();
				const auto id = request->m_identityRemote.GetSteamID64();
				const auto self = SteamUser()->GetSteamID();
				const auto host = lobby_.IsValid() ? SteamMatchmaking()->GetLobbyOwner(lobby_) : CSteamID{};
				if (relay_enabled_ && (host == self || host.ConvertToUint64() == id) && std::find(members.begin(), members.end(), id) != members.end())
					SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
				else SteamNetworkingMessages()->CloseSessionWithUser(request->m_identityRemote);
			}

			void relay_failed(SteamNetworkingMessagesSessionFailed_t* result)
			{
				console::warn("Steam transport: peer %llu failed (%d): %s\n", result->m_info.m_identityRemote.GetSteamID64(),
					result->m_info.m_eEndReason, result->m_info.m_szEndDebug);
			}

			void pump_transport()
			{
				if (!relay_enabled_ || !SteamUser()->BLoggedOn()) return;
				using title_address = DWORD(WINAPI*)(void*);
				const auto get_address = reinterpret_cast<title_address>(GetProcAddress(GetModuleHandleA("xlive.dll"), MAKEINTRESOURCEA(73)));
				std::array<unsigned char, 36> address{}; uint16 network_port = 0;
				const bool address_ready = get_address && get_address(address.data());
				if (address_ready) std::memcpy(&network_port, address.data() + 8, 2);
				std::vector<steam_ingress::identity> identities;
				auto* mm = SteamMatchmaking();
				const auto self = SteamUser()->GetSteamID();
				if (lobby_.IsValid() && address_ready)
				{
					// SDK member data belongs to the authenticated account, unlike
					// owner-editable lobby data or a claim inside an incoming packet.
					constexpr char hex[] = "0123456789abcdef";
					std::string encoded;
					for (const auto byte : address) { encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
					if (encoded != published_native_identity_)
					{ mm->SetLobbyMemberData(lobby_, "csl_native_identity", encoded.c_str()); published_native_identity_ = encoded; }
					identities.push_back({self.ConvertToUint64(), address});
					for (const auto peer : relay_members())
					{
						const auto* member_value = mm->GetLobbyMemberData(lobby_, CSteamID(peer), "csl_native_identity");
						if (!member_value || strnlen(member_value, 73) != 72) continue;
						const std::string value(member_value, 72);
						if (value.size() != 72) continue;
						steam_ingress::identity id{peer, {}};
						bool valid = true;
						for (std::size_t i = 0; i < id.native.size(); ++i)
						{
							unsigned byte = 0;
							const auto parsed = std::from_chars(value.data() + i * 2, value.data() + i * 2 + 2, byte, 16);
							if (parsed.ec != std::errc{} || parsed.ptr != value.data() + i * 2 + 2) { valid = false; break; }
							id.native[i] = static_cast<std::uint8_t>(byte);
						}
						if (valid && std::any_of(id.native.begin() + 10, id.native.end(), [](auto b) { return b != 0; })) identities.push_back(id);
					}
				}
				if (!transport_.configure(lobby_.ConvertToUint64(), ntohs(network_port), relay_members(),
					self.ConvertToUint64(), lobby_.IsValid() ? mm->GetLobbyOwner(lobby_).ConvertToUint64() : 0, std::move(identities))) return;
				auto* messages = SteamNetworkingMessages();
				transport_.pump([messages](const std::uint64_t id, const std::span<const std::uint8_t> bytes) {
					SteamNetworkingIdentity peer{}; peer.SetSteamID64(id);
					return messages->SendMessageToUser(peer, bytes.data(), static_cast<uint32>(bytes.size()),
						k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, relay_channel) == k_EResultOK;
				});
				std::array<SteamNetworkingMessage_t*, 32> batch{};
				for (int budget = 0; budget < 128; budget += 32)
				{
					const auto count = messages->ReceiveMessagesOnChannel(relay_channel, batch.data(), static_cast<int>(batch.size()));
					for (int i = 0; i < count; ++i)
					{
						const auto* message = batch[i];
						if (message->m_cbSize > 1 && message->m_cbSize <= 65531)
							transport_.receive(message->m_identityPeer.GetSteamID64(), {static_cast<const std::uint8_t*>(message->m_pData), static_cast<std::size_t>(message->m_cbSize)});
						batch[i]->Release();
					}
					if (count < static_cast<int>(batch.size())) break;
				}
			}

		public:
			void transport_status() const
			{
				console::info("Steam transport: sent=%llu received=%llu rejected=%llu failed=%llu\n", transport_.sent, transport_.received, transport_.rejected, transport_.failed);
				if (!active_ || !relay_enabled_) return;
				for (const auto id : relay_members())
				{
					SteamNetworkingIdentity peer{}; peer.SetSteamID64(id); SteamNetConnectionInfo_t info{};
					const auto state = SteamNetworkingMessages()->GetSessionConnectionInfo(peer, &info, nullptr);
					console::info("Steam transport: peer=%llu state=%d relayPOP=%u\n", id, state, info.m_idPOPRelay);
				}
			}
		private:

			void finish_find(const char* error)
			{
				cancel_find();
				find_match_error(error);
			}

			void update_identity(const bool starting = false)
			{
				// Never rewrite real GFWL's signed-in account. The local shim keeps
				// its XUID/profile stable; only the native display cache is synced.
				if (!xlive::is_local_shim_loaded()) return;
				auto* name = game::Dvar_FindVar("name");
				if (!name || static_cast<unsigned char>(name->type) != game::DVAR_TYPE_STRING) return;
				if (!identity_reserved_)
				{
					// GFWL SDK XUserGetXUID, preserved ordinal 5261: reserve identity
					// before changing the display name, not from the Steam persona.
					using get_xuid = DWORD(WINAPI*)(DWORD, ULONGLONG*);
					const auto get = reinterpret_cast<get_xuid>(GetProcAddress(GetModuleHandleA("xlive.dll"), MAKEINTRESOURCEA(5261)));
					ULONGLONG xuid = 0;
					if (!get || get(0, &xuid) != ERROR_SUCCESS || !xuid) return;
					identity_reserved_ = true;
				}
				const auto state = *reinterpret_cast<const int*>(game::game_offset(0x111F45F8));
				const bool match = starting || state >= 3 || running_server() || host_launch_started_ != 0;
				const auto* automatic = game::Dvar_FindVar("steam_useSteamName");
				const bool steam_name = automatic && automatic->current.enabled && active_ && SteamUser()->BLoggedOn();
				const std::string desired = steam_name ? SteamFriends()->GetPersonaName()
					: !manual_name_.empty() ? manual_name_ : name->current.string ? name->current.string : "Player";
				auto* cache = reinterpret_cast<char*>(game::game_offset(0x115D6A14));
				const char* baseline = *cache && std::memchr(cache, 0, 16) ? cache
					: name->current.string ? name->current.string : "Player";
				// Steam may initialize late, after a map is already active. In that
				// case retain the native name, not a newly fetched Steam persona.
				const auto& display = identity_.update(match && !identity_.initialized()
					? baseline : desired, match);
				// QoS 1024B670 caches XUserGetName at 115D6A14, 16 bytes, stride
				// 72; 103ED3E0 overwrites outgoing userinfo name from this cache.
				// Do not touch the adjacent XUID or relogin/recreate a profile.
				std::memset(cache, 0, 16); std::memcpy(cache, display.data(), display.size());
				name->flags = static_cast<game::dvar_flags>((static_cast<unsigned int>(name->flags)
					& ~static_cast<unsigned int>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched))
					| game::dvar_flags::saved | game::dvar_flags::user_info | (match ? game::dvar_flags::read_only : game::dvar_flags::none));
				if (!name->current.string || display != name->current.string) game::Dvar_SetString("name", display.c_str());
				if (active_ && lobby_.IsValid() && SteamUser()->BLoggedOn()
					&& display != SteamMatchmaking()->GetLobbyMemberData(lobby_, SteamUser()->GetSteamID(), "csl_display_name"))
					SteamMatchmaking()->SetLobbyMemberData(lobby_, "csl_display_name", display.c_str());
			}

			void update_presence()
			{
				const auto* detailed = game::Dvar_FindVar("steam_presenceStatus");
				std::string detail;
				if (detailed && detailed->current.enabled)
				{
					const auto* ingame = game::Dvar_FindVar("cl_ingame");
					if (running_server() || (ingame && ingame->current.enabled))
					{
						const auto* map = game::Dvar_FindVar("mapname");
						const auto* mode = game::Dvar_FindVar("g_gametype");
						if (map && mode && map->current.string && mode->current.string
							&& safe_host_token(map->current.string, 32) && safe_host_token(mode->current.string, 16))
							detail = std::string(mode->current.string) + " - " + map->current.string;
						else detail = "In match";
					}
					else if (lobby_.IsValid()) detail = "Lobby " + std::to_string(SteamMatchmaking()->GetNumLobbyMembers(lobby_)) + "/18";
					else detail = finding_ ? "Finding match" : "In menus";
				}
				presence_.update(detail);
			}

			bool eligible(CSteamID lobby)
			{
				auto* matchmaking = SteamMatchmaking();
				return lobby.IsValid() && lobby.IsLobby()
					&& matchmaking->GetLobbyOwner(lobby) != SteamUser()->GetSteamID()
					&& !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_product"), product)
					&& !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_protocol"), protocol)
					&& find_match_server_kind(matchmaking->GetLobbyData(lobby, "csl_server_kind"))
					&& (!std::strcmp(matchmaking->GetLobbyData(lobby, "csl_game_ready"), "1")
						|| !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_host_state"), "starting")
						|| !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_host_state"), "waiting"))
					&& matches_rotation(find_rotation_, matchmaking->GetLobbyData(lobby, "csl_map"),
						matchmaking->GetLobbyData(lobby, "csl_gametype"))
					&& relay_enabled_;
			}

			bool browser_eligible(CSteamID lobby) const
			{
				auto* mm = SteamMatchmaking();
				const std::string map = mm->GetLobbyData(lobby, "csl_map"), mode = mm->GetLobbyData(lobby, "csl_gametype");
				return lobby.IsValid() && lobby.IsLobby() && mm->GetLobbyOwner(lobby) != SteamUser()->GetSteamID()
					&& !std::strcmp(mm->GetLobbyData(lobby, "csl_product"), product)
					&& !std::strcmp(mm->GetLobbyData(lobby, "csl_protocol"), protocol)
					&& !std::strcmp(mm->GetLobbyData(lobby, "csl_game_ready"), "1")
					&& !std::strcmp(mm->GetLobbyData(lobby, "csl_host_state"), "running")
					&& browser_server_kind(mm->GetLobbyData(lobby, "csl_server_kind"))
					&& (browser_show_listen()
						|| !std::strcmp(mm->GetLobbyData(lobby, "csl_server_kind"), "dedicated"))
					&& map.size() < 32 && mode.size() < 16
					&& safe_host_token(map.c_str(), map.size() + 1) && safe_host_token(mode.c_str(), mode.size() + 1);
			}

			void next_candidate()
			{
				find_join_pending_ = false;
				while (finding_ && !candidates_.empty())
				{
					const auto candidate = candidates_.back(); candidates_.pop_back();
					if (!eligible(candidate)) continue;
					join(std::to_string(candidate.ConvertToUint64()));
					if (pending_) return;
				}
				if (finding_) create_find_host();
			}

			void create_find_host()
			{
				// No success/IO error is treated as a host. This path is reached only
				// after a completed search and unsuccessful compatible candidates.
				std::vector<rotation_entry> current;
				std::string name;
				if (!selected_rotation(current, name) || name != find_name_)
				{ finish_find("Playlist changed while searching. Please start Find Match again."); return; }
				host_entry_ = choose_host_entry();
				if (host_entry_ < 0 || static_cast<std::size_t>(host_entry_) >= current.size()
					|| !safe_host_token(current[host_entry_].map, sizeof(rotation_entry::map))
					|| !safe_host_token(current[host_entry_].gametype, sizeof(rotation_entry::gametype)))
				{ finish_find("Could not select a valid native playlist entry for hosting."); return; }
				find_rotation_ = std::move(current);
				find_host_pending_ = true; auto_create_requested_ = true;
				console::info("Steam Find Match: no host found; creating a public lobby for %s/%s.\n",
					find_rotation_[host_entry_].map, find_rotation_[host_entry_].gametype);
				create(true, "");
				if (!pending_)
				{
					find_host_pending_ = auto_create_requested_ = false;
					finish_find("Could not create the Steam host lobby. Check your Steam connection.");
				}
			}

			bool autostart_enabled() const
			{
				const auto* setting = game::Dvar_FindVar("sv_autostart");
				return setting && setting->current.enabled;
			}

			int lobby_rule(const char* name) const
			{
				const auto* setting = game::Dvar_FindVar(name);
				return setting && static_cast<unsigned char>(setting->type) == game::DVAR_TYPE_INT ? setting->current.integer : -1;
			}

			void update_countdown()
			{
				if (!waiting_host_ || !lobby_.IsValid() || pending_ || running_server()
					|| SteamMatchmaking()->GetLobbyOwner(lobby_) != SteamUser()->GetSteamID()) return;
				auto* mm = SteamMatchmaking();
				const int minimum = lobby_rule("party_minplayers"), maximum = lobby_rule("party_maxplayers");
				const int pre = lobby_rule("party_pregameStartTimerLength"), game = lobby_rule("party_gameStartTimerLength");
				const int lobby_time = lobby_rule("party_minLobbyTime");
				const auto* editing = game::Dvar_FindVar("party_editingsettings");
				const bool valid = minimum >= 1 && maximum >= minimum && maximum <= 18
					&& pre >= 0 && pre <= 99999 && game >= 0 && game <= 99999 && lobby_time >= 0 && lobby_time <= 2147483;
				const auto now = GetTickCount64();
				const bool allowed = valid && autostart_enabled() && mm->GetNumLobbyMembers(lobby_) >= minimum
					&& (!editing || !editing->current.enabled);
				const bool launch = countdown_.advance(now, allowed, pre, game, lobby_time);
				if (valid) mm->SetLobbyMemberLimit(lobby_, maximum);
				const bool published = mm->SetLobbyData(lobby_, "csl_minplayers", std::to_string(minimum).c_str())
					&& mm->SetLobbyData(lobby_, "csl_autostart", autostart_enabled() ? "1" : "0")
					&& mm->SetLobbyData(lobby_, "csl_timer", std::to_string(countdown_.seconds(now)).c_str())
					&& mm->SetLobbyData(lobby_, "csl_timer_visible", countdown_.visible(now) ? "1" : "0");
				// Never launch before clients can receive the timer state. Retry
				// publication next frame rather than pretending it succeeded.
				if (launch && published) start_match();
			}

			std::array<int, 2> vote_counts() const
			{
				std::array<int, 2> counts{};
				if (!lobby_.IsValid()) return counts;
				auto* mm = SteamMatchmaking();
				const std::string round = mm->GetLobbyData(lobby_, "csl_vote_round");
				for (int i = 0; i < mm->GetNumLobbyMembers(lobby_) && i < 18; ++i)
				{
					const std::string ballot = mm->GetLobbyMemberData(lobby_, mm->GetLobbyMemberByIndex(lobby_, i), "csl_vote");
					for (int choice = 0; choice < 2; ++choice)
						if (ballot == round + ":" + std::to_string(choice)) ++counts[choice];
				}
				return counts;
			}

			bool prepare_votes(const bool choose = false)
			{
				if (choose) host_entry_ = choose_host_entry();
				if (host_entry_ < 0 || static_cast<std::size_t>(host_entry_) >= find_rotation_.size()) return false;
				vote_entries_.fill(host_entry_);
				for (std::size_t i = 0; i < find_rotation_.size(); ++i)
					if (find_rotation_[i].weight > 0 && static_cast<int>(i) != host_entry_)
					{ vote_entries_[1] = static_cast<int>(i); break; }
				auto* mm = SteamMatchmaking();
				for (int choice = 0; choice < 2; ++choice)
				{
					const auto& entry = find_rotation_[vote_entries_[choice]];
					if (!safe_host_token(entry.map, sizeof(entry.map)) || !safe_host_token(entry.gametype, sizeof(entry.gametype))) return false;
					const auto suffix = std::to_string(choice);
					if (!mm->SetLobbyData(lobby_, ("csl_vote_map" + suffix).c_str(), entry.map)
						|| !mm->SetLobbyData(lobby_, ("csl_vote_mode" + suffix).c_str(), entry.gametype)) return false;
				}
				return mm->SetLobbyData(lobby_, "csl_map", find_rotation_[host_entry_].map)
					&& mm->SetLobbyData(lobby_, "csl_gametype", find_rotation_[host_entry_].gametype)
					&& mm->SetLobbyData(lobby_, "csl_vote_round", std::to_string(++vote_round_).c_str());
			}

			void persist_public_stats()
			{
				if (!playlist_lobby_) { stats_was_ingame_ = false; return; }
				const bool ingame = *reinterpret_cast<const int*>(game::game_offset(0x111F45F8)) == 10;
				const bool ended = stats_was_ingame_ && !ingame;
				stats_was_ingame_ = ingame;
				// QoS LiveStorage_SetStat 102402D0 sets +1 only after a real
				// downloaded profile (+0). Never invent either flag or a payload.
				const auto* flags = reinterpret_cast<const unsigned char*>(game::game_offset(0x116101C8));
				if (flags[0] && flags[1] && (ended || GetTickCount64() - last_stats_save_ >= 30000))
				{
					last_stats_save_ = GetTickCount64();
					// The native command preserves the checksum/8192-byte mpdata
					// serialization and reaches the shim's durable title storage.
					command::execute("uploadStats");
				}
			}

			void publish_roster()
			{
				const bool online = active_ && SteamUser()->BLoggedOn();
				auto* mm = SteamMatchmaking();
				const int count = online ? lobby_.IsValid() ? std::clamp(mm->GetNumLobbyMembers(lobby_), 0, 18) : 1 : 0;
				if (online && lobby_.IsValid())
				{
					const auto xp = local_rank_xp();
					const auto value = xp >= 0 ? std::to_string(xp) : "";
					if (value != mm->GetLobbyMemberData(lobby_, SteamUser()->GetSteamID(), "csl_rankxp"))
						mm->SetLobbyMemberData(lobby_, "csl_rankxp", value.c_str());
				}
				// Presentation-only adapter: never fabricate a native party or use
				// these row indices for LIVE kick/gamercard commands.
				for (int i = 0; i < 18; ++i)
				{
					std::string name;
					if (i < count)
					{
						const auto member = lobby_.IsValid() ? mm->GetLobbyMemberByIndex(lobby_, i) : SteamUser()->GetSteamID();
						const auto* display = lobby_.IsValid() ? mm->GetLobbyMemberData(lobby_, member, "csl_display_name") : nullptr;
						const auto* persona = member == SteamUser()->GetSteamID() ? SteamFriends()->GetPersonaName() : SteamFriends()->GetFriendPersonaName(member);
						name = steam_friends::label(display && *display ? display : persona);
						name += lobby_rank(member);
						if (lobby_.IsValid() && member == mm->GetLobbyOwner(lobby_)) name += " (HOST)";
					}
					game::Dvar_SetFromStringByName(("csl_roster_name" + std::to_string(i)).c_str(), name.c_str());
					menu_file::set_match_lobby_text("member" + std::to_string(i), name);
				}
				game::Dvar_SetFromStringByName("csl_roster_count", std::to_string(count).c_str());
				game::Dvar_SetFromStringByName("csl_roster_active", online ? "1" : "0");
				// Matchmaking membership is not a private/invite party. Until a
				// separate private-party transport is implemented, that screen has
				// only the local user; never promote matched strangers into it.
				game::Dvar_SetFromStringByName("csl_party_count", online ? "1" : "0");
				game::Dvar_SetFromStringByName("csl_party_name0", online
					? (steam_friends::label(SteamFriends()->GetPersonaName()) + lobby_rank(SteamUser()->GetSteamID())).c_str() : "");
			}

			int local_rank_xp() const
			{
				if (!*reinterpret_cast<const unsigned char*>(game::game_offset(0x116101C8))) return -1;
				// QoS LB_Init's fifth playerStatsTable mapping is RANKXP, not a
				// leaderboard position. Same native stat addressing as 102403D0.
				const auto index = reinterpret_cast<const unsigned*>(game::game_offset(0x11634E08))[4];
				if (index >= 3498) return -1;
				return index < 2000 ? *reinterpret_cast<const unsigned char*>(game::game_offset(0x1160BFB4) + index)
					: std::max(0, *reinterpret_cast<const int*>(game::game_offset(0x1160A844) + index * 4));
			}

			std::string lobby_rank(const CSteamID member) const
			{
				int xp = member == SteamUser()->GetSteamID() ? local_rank_xp() : -1;
				if (xp < 0 && lobby_.IsValid())
				{
					const auto* text = SteamMatchmaking()->GetLobbyMemberData(lobby_, member, "csl_rankxp");
					const auto size = text ? strnlen(text, 12) : 0;
					if (!size || size > 10) return {};
					const auto parsed = std::from_chars(text, text + size, xp);
					if (parsed.ec != std::errc{} || parsed.ptr != text + size || xp < 0) return {};
				}
				if (xp < 0) return {};
				// QoS 102FAE70 matches KisakCOD CL_GetRankForXp; 102FB040 uses
				// QoS rankTable column 14 for the displayed progression rank.
				const auto rank = reinterpret_cast<int(__cdecl*)(int)>(game::game_offset(0x102FAE70))(xp);
				const auto* label = reinterpret_cast<const char*(__cdecl*)(int)>(game::game_offset(0x102FB040))(rank);
				if (!label || !*label || strnlen(label, 64) >= 64) return {};
				const auto translated = gametypes::labels::resolve(label, [](const char* key) {
					return reinterpret_cast<const char*(__cdecl*)(const char*)>(game::game_offset(0x103C9F20))(key);
				});
				return " [" + steam_friends::label(translated.c_str()) + "]";
			}
			void update_lobby_screen()
			{
				if (leave_requested_) { menu_file::set_match_lobby(false); return; }
				if (!lobby_.IsValid()) { menu_file::set_match_lobby(false); return; }
				auto* mm = SteamMatchmaking();
				// Only playlist lobbies own the persistent match/vote screen.
				if (!*mm->GetLobbyData(lobby_, "csl_vote_round")) return;
				if (!std::strcmp(mm->GetLobbyData(lobby_, "csl_host_state"), "closed")
					&& *reinterpret_cast<const int*>(game::game_offset(0x111F45F8)) < 3)
				{
					leave();
					command::execute("party_leavematchlobby; sf_open mpxblivemain");
					return;
				}
				menu_file::set_match_lobby(true);
				const auto clean = [](const char* text) {
					std::string result;
					for (const auto* p = text; p && *p && result.size() < 60; ++p)
						if (static_cast<unsigned char>(*p) >= 32 && *p != '^') result += *p;
					return result;
				};
				const auto counts = vote_counts();
				for (int i = 0; i < 2; ++i)
				{
					const auto suffix = std::to_string(i);
					const std::string map = clean(mm->GetLobbyData(lobby_, ("csl_vote_map" + suffix).c_str()));
					const std::string mode = clean(mm->GetLobbyData(lobby_, ("csl_vote_mode" + suffix).c_str()));
					const bool valid = map.size() < 32 && mode.size() < 16
						&& safe_host_token(map.c_str(), map.size() + 1) && safe_host_token(mode.c_str(), mode.size() + 1);
					menu_file::set_match_lobby_text("vote_map" + suffix, valid ? map : "");
					menu_file::set_match_lobby_text("vote" + suffix, valid ? vote_display_name(map, true) + "\n" + vote_display_name(mode, false) : "");
					menu_file::set_match_lobby_text("vote_count" + suffix, std::to_string(counts[i]));
				}
				const bool owner = mm->GetLobbyOwner(lobby_) == SteamUser()->GetSteamID();
				game::Dvar_SetFromStringByName("csl_lobby_owner", owner ? "1" : "0");
				game::Dvar_SetFromStringByName("csl_lobby_can_start", owner && waiting_host_ && !pending_ && !running_server() ? "1" : "0");
				const std::string state = mm->GetLobbyData(lobby_, "csl_host_state");
				const bool timer = !std::strcmp(mm->GetLobbyData(lobby_, "csl_timer_visible"), "1");
				menu_file::set_match_lobby_text("state", state == "closed" ? "Host left"
					: state == "running" ? "Game in progress"
					: state != "waiting" ? "Starting game..."
					: timer ? "Time left: " + clean(mm->GetLobbyData(lobby_, "csl_timer")) + "s"
					: !std::strcmp(mm->GetLobbyData(lobby_, "csl_autostart"), "1") ? "Waiting for players..."
					: "Waiting for host...");
				for (int i = 0; i < 18; ++i)
				{
					std::string name;
					if (i < mm->GetNumLobbyMembers(lobby_))
					{
						const auto member = mm->GetLobbyMemberByIndex(lobby_, i);
						const auto* display = mm->GetLobbyMemberData(lobby_, member, "csl_display_name");
						name = steam_friends::label(display && *display ? display : SteamFriends()->GetFriendPersonaName(member));
						name += lobby_rank(member);
						if (member == mm->GetLobbyOwner(lobby_)) name += " (HOST)";
					}
					menu_file::set_match_lobby_text("member" + std::to_string(i), name);
				}
			}

			void launch_find_host(const bool manual = false)
			{
				std::vector<rotation_entry> current;
				std::string name;
				if (!selected_rotation(current, name) || name != find_name_
					|| host_entry_ < 0 || static_cast<std::size_t>(host_entry_) >= current.size()
					|| std::strcmp(current[host_entry_].map, find_rotation_[host_entry_].map)
					|| std::strcmp(current[host_entry_].gametype, find_rotation_[host_entry_].gametype)
					|| !apply_host_rules(host_entry_))
				{
					leave(); find_match_error("The selected playlist changed or native host setup failed. Please retry Find Match."); return;
				}
				if (!manual)
				{
					if (!SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "waiting"))
					{ leave(); find_match_error("Could not publish the waiting lobby."); return; }
					finding_ = false; waiting_host_ = true;
					countdown_.reset(); countdown_.opened = GetTickCount64();
					update_countdown();
					console::info("Steam: lobby waiting for players; automatic starts use the native minimum-player and countdown settings.\n");
					return;
				}
				// Use QoS' working local session/gameplay transport, independent of
				// which frontend menu initiated the Steam host. No System Link menu
				// navigation or fake session/lobby state is required.
				if (!SteamMatchmaking()->SetLobbyData(lobby_, "csl_map", current[host_entry_].map)
					|| !SteamMatchmaking()->SetLobbyData(lobby_, "csl_gametype", current[host_entry_].gametype)
					|| !SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "starting"))
				{ leave(); find_match_error("Could not publish match startup."); return; }
				finding_ = false; find_host_pending_ = false; waiting_host_ = false;
				countdown_.reset();
				SteamMatchmaking()->SetLobbyData(lobby_, "csl_timer_visible", "0");
				update_identity(); // capture the latest lobby persona before freezing
				host_launch_started_ = GetTickCount64();
				update_identity(true);
				// Progression policy is separate from the System Link transport.
				// QoS GSC otherwise treats every Steam public match as offline.
				game::Dvar_SetFromStringByName("csl_public_match", "1");
				command::execute("xstopparty; xstopprivateparty; systemlink 1; onlinegame 0; splitscreen 0; xblive_privatematch 0; xblive_rankedmatch 0; uiscript \"uiScript StartServer\"");
				console::info("Steam Find Match: native match startup requested for %s/%s.\n",
					current[host_entry_].map, current[host_entry_].gametype);
			}

			void maintain_auto_lobby()
			{
				// An explicit map/Start Match action also ends pre-game waiting.
				if (waiting_host_ && running_server()) waiting_host_ = false;
				if (host_resolve_at_ && finding_ && GetTickCount64() >= host_resolve_at_)
				{
					host_resolve_at_ = 0; resolving_find_host_ = true;
					list();
					if (!pending_) finish_find("Steam could not confirm the host reservation. Please retry.");
				}
				const bool running = running_server();
				if (!running) suppress_auto_lobby_ = false;
				if (auto_lobby_ && running) { auto_match_started_ = true; host_launch_started_ = 0; }
				if (auto_lobby_ && !running && auto_match_started_ && !pending_)
				{
					auto_match_started_ = false;
					if (!playlist_lobby_)
					{
						// A System Link/manual host's published Steam lobby is a real
						// control-plane session too. End gameplay, not membership.
						SteamMatchmaking()->SetLobbyData(lobby_, "csl_game_ready", "0");
						SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "waiting");
						published_ready_ = false;
						return;
					}
					if (!SteamMatchmaking()->SetLobbyData(lobby_, "csl_game_ready", "0") || !prepare_votes(true)
						|| !SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "waiting"))
					{ leave(); find_match_error("Could not prepare the next lobby vote."); return; }
					waiting_host_ = true; published_ready_ = false;
					// Reapply the new round's playlist rules before reading its gates.
					if (!apply_host_rules(host_entry_)) { leave(); find_match_error("Could not apply next-round rules."); return; }
					countdown_.reset(); countdown_.opened = GetTickCount64();
				}
				update_countdown();
				if (host_launch_started_ && !running && GetTickCount64() - host_launch_started_ >= 60000)
				{
					host_launch_started_ = 0; leave();
					find_match_error("The native match did not start. Check the selected map files and console log."); return;
				}
				// A deliberate pre-game lobby has no startup deadline. Restart the
				// deadline while waiting so the host still gets a full minute to load.
				if (lobby_.IsValid() && SteamMatchmaking()->GetLobbyOwner(lobby_) != SteamUser()->GetSteamID()
					&& !std::strcmp(SteamMatchmaking()->GetLobbyData(lobby_, "csl_host_state"), "waiting"))
				{
					waiting_game_since_ = GetTickCount64();
					attempted_ip_ = 0; attempted_port_ = 0;
				}
				if (waiting_game_since_ && !attempted_ip_ && GetTickCount64() - waiting_game_since_ >= 60000)
				{
					leave(); find_match_error("The Steam host did not publish a running match. Please retry Find Match."); return;
				}
				const auto* automatic = game::Dvar_FindVar("steam_autoLobby");
				if (!running || !automatic || !automatic->current.enabled || suppress_auto_lobby_
					|| pending_ || finding_ || lobby_.IsValid() || !SteamUser()->BLoggedOn()
					|| !xlive::is_local_shim_loaded() || GetTickCount64() - last_auto_attempt_ < 30000) return;
				last_auto_attempt_ = GetTickCount64(); auto_create_requested_ = true;
				create(true, "");
				if (!pending_) auto_create_requested_ = false;
			}

			bool running_server() const
			{
				const auto* running = game::Dvar_FindVar("sv_running");
				return running && running->current.enabled;
			}

			const char* host_kind() const
			{
				const auto* dedicated = game::Dvar_FindVar("dedicated");
				return dedicated && static_cast<unsigned char>(dedicated->type) == game::DVAR_TYPE_INT
					&& dedicated->current.integer > 0 ? "dedicated" : "listen";
			}

			void publish_game()
			{
				if (!lobby_.IsValid() || !SteamUser()->BLoggedOn()
					|| SteamMatchmaking()->GetLobbyOwner(lobby_) != SteamUser()->GetSteamID()) return;
				auto* matchmaking = SteamMatchmaking();
				if (!running_server() || !xlive::is_local_shim_loaded())
				{
					if (!auto_lobby_ && !waiting_host_ && !host_launch_started_ && !host_resolve_at_ && !resolving_find_host_)
						matchmaking->SetLobbyData(lobby_, "csl_host_state", "stopped");
					if (published_ready_ && matchmaking->SetLobbyData(lobby_, "csl_game_ready", "0"))
						published_ready_ = false;
					return;
				}
				const auto* map = game::Dvar_FindVar("mapname");
				const auto* mode = game::Dvar_FindVar("g_gametype");
				if (!map || !mode || static_cast<unsigned char>(map->type) != game::DVAR_TYPE_STRING
					|| static_cast<unsigned char>(mode->type) != game::DVAR_TYPE_STRING
					|| !map->current.string || !mode->current.string) return;
				if (!matchmaking->SetLobbyData(lobby_, "csl_map", map->current.string)
					|| !matchmaking->SetLobbyData(lobby_, "csl_gametype", mode->current.string)
					|| !matchmaking->SetLobbyData(lobby_, "csl_server_kind", host_kind())
					|| !matchmaking->SetLobbyData(lobby_, "csl_host_state", "running")) return;
				// GFWL SDK XNADDR is 36 bytes; wPortOnline is network-order at +8.
				// Use the actual shim bind port, not Steam's server/query defaults.
				using title_address = DWORD(WINAPI*)(void*);
				const auto get_address = reinterpret_cast<title_address>(GetProcAddress(
					GetModuleHandleA("xlive.dll"), MAKEINTRESOURCEA(73)));
				std::array<unsigned char, 36> address{};
				uint16 network_port = 0;
				if (!get_address || !get_address(address.data())) return;
				std::memcpy(&network_port, address.data() + 8, sizeof(network_port));
				const auto port = ntohs(network_port);
				if (!relay_enabled_ || !port) return;
				if (published_ready_ && published_ip_ == host_ip_ && published_port_ == port) return;
				if (!matchmaking->SetLobbyData(lobby_, "csl_game_port", std::to_string(port).c_str()) ||
					!matchmaking->SetLobbyData(lobby_, "csl_game_epoch", std::to_string(GetTickCount64()).c_str()) ||
					!matchmaking->SetLobbyData(lobby_, "csl_game_ready", "1")) return;
				published_ip_ = host_ip_; published_port_ = port; published_ready_ = true;
				console::info("Steam: published running QoS host via SDR (game port %u).\n", port);
			}

			void connect_game()
			{
				if (!lobby_.IsValid() || !SteamUser()->BLoggedOn()) return;
				auto* matchmaking = SteamMatchmaking();
				if (matchmaking->GetLobbyOwner(lobby_) == SteamUser()->GetSteamID()) return;
				if (std::strcmp(matchmaking->GetLobbyData(lobby_, "csl_product"), product)
					|| std::strcmp(matchmaking->GetLobbyData(lobby_, "csl_protocol"), protocol)
					|| std::strcmp(matchmaking->GetLobbyData(lobby_, "csl_game_ready"), "1")) return;
				if (!relay_enabled_) return;
				const std::string port_text = matchmaking->GetLobbyData(lobby_, "csl_game_port");
				unsigned int remote_port = 0;
				const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), remote_port);
				if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || !remote_port || remote_port > 65535) return;
				const uint32 ip = INADDR_LOOPBACK;
				SteamNetworkingIdentity remote{}; remote.SetSteamID(matchmaking->GetLobbyOwner(lobby_));
				const auto connection = SteamNetworkingMessages()->GetSessionConnectionInfo(remote, nullptr, nullptr);
				if (connection != k_ESteamNetworkingConnectionState_Connected)
				{
					// Establish the SDR route before starting QoS's five-second query.
					// One-byte zero messages establish transport only, never enter QoS.
					if (connection == k_ESteamNetworkingConnectionState_None || connection < 0)
					{
						const std::uint8_t hello = 0;
						SteamNetworkingMessages()->SendMessageToUser(remote, &hello, 1,
							k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession, relay_channel);
					}
					return;
				}
				const uint16 port = transport_.endpoint(matchmaking->GetLobbyOwner(lobby_).ConvertToUint64(), static_cast<uint16>(remote_port));
				if (!port) return;
				// A new round can reuse the same UDP endpoint. Do not mistake it
				// for the connection already attempted in the previous round.
				const std::string round = matchmaking->GetLobbyData(lobby_, "csl_game_epoch");
				if (round != observed_round_)
				{ observed_round_ = round; attempted_ip_ = 0; attempted_port_ = 0; }
				if (attempted_ip_ == ip && attempted_port_ == port) return;
				attempted_ip_ = ip; attempted_port_ = port;
				waiting_game_since_ = 0;
				if (running_server() || !xlive::is_local_shim_loaded())
				{
					console::warn("Steam: stop your own match and deploy the offline xlive shim before joining; then leave/rejoin the lobby.\n");
					return;
				}
				// Only numeric SDK fields become a command; never execute lobby text.
				const auto endpoint = std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255)
					+ "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255)
					+ ":" + std::to_string(port);
				console::info("Steam: joining QoS host %s via validated session query.\n", endpoint.c_str());
				update_identity();
				command::execute("connectIP " + endpoint);
			}

			void game_created(LobbyGameCreated_t* result)
			{
				if (result->m_ulSteamIDLobby == lobby_.ConvertToUint64()) connect_game();
			}

			void unavailable(const char* reason)
			{
				command::set_steam_identity(0);
				if (last_init_error_ != reason)
				{
					last_init_error_ = reason;
					console::warn("Steam online services unavailable: %s (retrying automatically).\n", reason);
				}
				warn_sign_in();
			}

			void warn_sign_in()
			{
				if (offline_warning_shown_) return;
				offline_warning_shown_ = true;
				console::warn("To access online services, you must be signed into Steam. Offline play remains available.\n");
			}

			void check_sign_in()
			{
				command::set_steam_identity(SteamUser()->BLoggedOn() ? SteamUser()->GetSteamID().ConvertToUint64() : 0);
				if (SteamUser()->BLoggedOn()) offline_warning_shown_ = false;
				else { close_transport(); warn_sign_in(); }
			}

			void invited(GameLobbyJoinRequested_t* result)
			{
				// User explicitly accepted the Steam invite. Never execute lobby text
				// as an engine command or silently leave an existing party.
				join(std::to_string(result->m_steamIDLobby.ConvertToUint64()));
			}

			void kicked(LobbyKicked_t* result)
			{
				if (result->m_ulSteamIDLobby == lobby_.ConvertToUint64())
				{
					close_transport();
					lobby_ = CSteamID();
					menu_file::set_match_lobby(false);
					waiting_host_ = false;
					playlist_lobby_ = auto_lobby_ = auto_match_started_ = false;
					observed_round_.clear();
					waiting_game_since_ = host_launch_started_ = 0;
					host_ip_ = published_ip_ = attempted_ip_ = 0;
					host_port_ = published_port_ = attempted_port_ = 0;
					published_ready_ = false;
					console::warn("Steam: lobby membership lost.\n");
				}
			}

			bool ready()
			{
				if (!active_ || stopped_ || !SteamUser()->BLoggedOn())
				{
					console::warn("To access online services, you must be signed into Steam. Use steam_init to retry after signing in.\n");
					return false;
				}
				if (pending_) { console::warn("Steam: lobby operation already pending.\n"); return false; }
				if (!relay_enabled_) { console::warn("Steam gameplay unavailable: matching security-hardened xlive.dll and SDR support are required.\n"); return false; }
				return true;
			}

			bool begin(const SteamAPICall_t call)
			{
				if (call == k_uAPICallInvalid) { console::error("Steam: lobby request could not be submitted.\n"); return false; }
				pending_ = true;
				return true;
			}

			void created(LobbyCreated_t* result, const bool failed)
			{
				pending_ = false;
				const bool find_host = find_host_pending_;
				find_host_pending_ = false;
				if (failed || result->m_eResult != k_EResultOK)
				{
					console::error("Steam: lobby creation failed (IO=%d result=%d).\n", failed, failed ? -1 : static_cast<int>(result->m_eResult));
					auto_create_requested_ = cancelled_auto_create_ = false;
					if (find_host && finding_) finish_find("Steam could not create a match lobby. Please try again.");
					return;
				}
				lobby_ = CSteamID(result->m_ulSteamIDLobby);
				if (leave_requested_) { leave(); return; }
				auto_lobby_ = auto_create_requested_; auto_create_requested_ = false;
				if (cancelled_auto_create_)
				{ cancelled_auto_create_ = false; leave(); return; }
				auto* matchmaking = SteamMatchmaking();
				// Fail closed if the discovery namespace cannot be published.
				if (!matchmaking->SetLobbyData(lobby_, "csl_product", product) ||
					!matchmaking->SetLobbyData(lobby_, "csl_protocol", protocol) ||
					!matchmaking->SetLobbyData(lobby_, "csl_server_kind", host_kind()) ||
					!matchmaking->SetLobbyData(lobby_, "csl_game_ready", "0") ||
					!matchmaking->SetLobbyData(lobby_, "name", "Project: Consolation"))
				{
					leave();
					console::error("Steam: lobby metadata publication failed; left lobby.\n");
					if (find_host) find_match_error("Steam could not publish the host lobby metadata.");
					return;
				}
				if (find_host && finding_)
				{
					playlist_lobby_ = true;
					if (!prepare_votes()) { leave(); find_match_error("Could not publish the map vote."); return; }
					// Reserve this map/mode before loading so another Find Match can
					// join the lobby and wait, rather than treating startup as no host.
					using title_address = DWORD(WINAPI*)(void*);
					const auto get_address = reinterpret_cast<title_address>(GetProcAddress(
						GetModuleHandleA("xlive.dll"), MAKEINTRESOURCEA(73)));
					std::array<unsigned char, 36> address{};
					uint16 network_port = 0;
					if (!get_address || !get_address(address.data()))
					{ leave(); find_match_error("The local session address is unavailable."); return; }
					std::memcpy(&network_port, address.data() + 8, sizeof(network_port));
					if (!matchmaking->SetLobbyData(lobby_, "csl_map", find_rotation_[host_entry_].map)
						|| !matchmaking->SetLobbyData(lobby_, "csl_gametype", find_rotation_[host_entry_].gametype)
						|| !matchmaking->SetLobbyData(lobby_, "csl_host_state", "starting"))
					{ leave(); find_match_error("Could not publish the planned match."); return; }
					if (!matchmaking->SetLobbyData(lobby_, "csl_game_port", std::to_string(ntohs(network_port)).c_str()))
					{ leave(); find_match_error("Could not publish the Steam transport port."); return; }
					// Let reservations become visible before electing a host. If two
					// players search together, the lower visible owner wins; existing
					// running matches always take precedence. This is a bounded lobby
					// coordination pass, not a claimed authoritative backend election.
					host_resolve_at_ = GetTickCount64() + 5000;
				}
				status();
				publish_game();
				if (!published_ready_) console::info("Steam: lobby created; waiting for native match startup.\n");
			}

			void listed(LobbyMatchList_t* result, const bool failed)
			{
				pending_ = false;
				if (leave_requested_) { browser_pending_ = false; leave(); return; }
				if (browser_pending_)
				{
					browser_pending_ = false; browser_rows_.clear();
					if (failed) { console::warn("Server Browser: Steam search failed.\n"); return; }
					auto* mm = SteamMatchmaking();
					const auto clean = [](const char* text) {
						std::string result;
						for (const auto* p = text; p && *p && result.size() < 48; ++p)
							if (static_cast<unsigned char>(*p) >= 32 && static_cast<unsigned char>(*p) < 127
								&& !std::strchr("|^<>&\"\\", *p)) result += *p;
						return result;
					};
					for (uint32 i = 0; i < result->m_nLobbiesMatching && i < 50; ++i)
					{
						const auto lobby = mm->GetLobbyByIndex(static_cast<int>(i));
						if (!browser_eligible(lobby)) continue;
						const auto row = clean(SteamFriends()->GetFriendPersonaName(mm->GetLobbyOwner(lobby)))
							+ "|" + clean(mm->GetLobbyData(lobby, "csl_map")) + "|"
							+ std::to_string(mm->GetNumLobbyMembers(lobby)) + "/" + std::to_string(mm->GetLobbyMemberLimit(lobby))
							+ "|" + clean(mm->GetLobbyData(lobby, "csl_gametype"));
						game::Dvar_SetFromStringByName(("csl_browser_row" + std::to_string(browser_rows_.size())).c_str(), row.c_str());
						browser_rows_.push_back(lobby);
					}
					game::Dvar_SetFromStringByName("csl_browser_count", std::to_string(browser_rows_.size()).c_str());
					return;
				}
				if (resolving_find_host_)
				{
					resolving_find_host_ = false;
					if (failed) { finish_find("Steam host reservation search failed. Please retry."); return; }
					CSteamID target;
					uint64 best = UINT64_MAX;
					bool best_running = false;
					for (uint32 i = 0; i < result->m_nLobbiesMatching && i < 50; ++i)
					{
						const auto candidate = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i));
						if (!eligible(candidate)) continue;
						const auto owner = SteamMatchmaking()->GetLobbyOwner(candidate).ConvertToUint64();
						const bool running = !std::strcmp(SteamMatchmaking()->GetLobbyData(candidate, "csl_game_ready"), "1");
						if (prefer_host(running, owner, SteamUser()->GetSteamID().ConvertToUint64(), best_running, best))
						{ target = candidate; best = owner; best_running = running; }
					}
					if (target.IsValid())
					{
						SteamMatchmaking()->LeaveLobby(lobby_); lobby_ = CSteamID();
						auto_lobby_ = auto_match_started_ = false;
						join(std::to_string(target.ConvertToUint64()));
						if (!pending_) finish_find("Could not join the selected Steam host. Please retry.");
					}
					else launch_find_host();
					return;
				}
				if (finding_)
				{
					if (failed) { finish_find("Steam lobby search failed. Please try again."); return; }
					// Steam returns sorted results; reverse storage preserves that order.
					for (uint32 i = result->m_nLobbiesMatching < 50 ? result->m_nLobbiesMatching : 50; i > 0; --i)
					{
						const auto candidate = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i - 1));
						if (eligible(candidate)) candidates_.push_back(candidate);
					}
					next_candidate();
					return;
				}
				if (failed) { console::error("Steam: lobby search failed.\n"); return; }
				console::info("Steam: %u matching Consolation lobbies.\n", result->m_nLobbiesMatching);
				for (uint32 i = 0; i < result->m_nLobbiesMatching && i < 50; ++i)
				{
					const auto lobby = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i));
					console::info("  %llu (%d/%d members)\n", lobby.ConvertToUint64(),
						SteamMatchmaking()->GetNumLobbyMembers(lobby), SteamMatchmaking()->GetLobbyMemberLimit(lobby));
				}
			}

			void joined(LobbyEnter_t* result, const bool failed)
			{
				pending_ = false;
				const bool find_join = find_join_pending_;
				find_join_pending_ = false;
				if (failed || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
				{
					console::error("Steam: lobby join failed (IO=%d response=%u).\n", failed, failed ? 0u : result->m_EChatRoomEnterResponse);
					cancelled_find_join_ = false;
					if (finding_ && find_join) next_candidate();
					return;
				}
				lobby_ = CSteamID(result->m_ulSteamIDLobby);
				if (leave_requested_) { leave(); return; }
				if (cancelled_find_join_)
				{
					cancelled_find_join_ = false; leave(); return;
				}
				if (finding_ && find_join && !eligible(lobby_))
				{
					// Leave this stale/full/changed candidate without cancelling the search.
					SteamMatchmaking()->LeaveLobby(lobby_); lobby_ = CSteamID();
					next_candidate(); return;
				}
				if (std::strcmp(SteamMatchmaking()->GetLobbyData(lobby_, "csl_product"), product) ||
					std::strcmp(SteamMatchmaking()->GetLobbyData(lobby_, "csl_protocol"), protocol))
				{
					leave();
					console::error("Steam: incompatible lobby; left without connecting to gameplay.\n");
					return;
				}
				// Guests need the same retained-lobby classification as the creator.
				playlist_lobby_ = *SteamMatchmaking()->GetLobbyData(lobby_, "csl_vote_round") != 0;
				game::Dvar_SetFromStringByName("csl_public_match", playlist_lobby_ ? "1" : "0");
				update_lobby_screen();
				status();
				finding_ = false; candidates_.clear();
				connect_game();
				if (!attempted_ip_)
				{
					waiting_game_since_ = GetTickCount64();
					console::info("Steam: lobby joined; waiting for the host to start its QoS match.\n");
				}
			}
		};
	}
#endif

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
#ifdef CSL_STEAMWORKS
			state_ = std::make_shared<service>();
			const std::weak_ptr<service> weak = state_;
			party_go_action = [weak]() {
				if (const auto state = weak.lock())
				{
					std::lock_guard lock(state->mutex);
					return state->party_go();
				}
				return false;
			};
			const auto enqueue = [weak](const std::function<void(service&)>& action)
			{
				scheduler::once([weak, action]()
				{
					if (const auto state = weak.lock())
					{
						std::lock_guard lock(state->mutex);
						action(*state);
					}
				}, scheduler::main);
			};
			command::add("steam_init", [enqueue]() { enqueue([](service& s) { s.initialize(); }); });
			command::add("steam_status", [enqueue]() { enqueue([](service& s) { s.status(); }); });
			command::add("steam_presence_status", [enqueue]() { enqueue([](service& s) { s.presence_status(); }); });
			command::add("steam_presence_retry", [enqueue]() { enqueue([](service& s) { s.presence_retry(); }); });
			command::add("steam_transport_status", [enqueue]() { enqueue([](service& s) { s.transport_status(); }); });
			friends_menu_action = [enqueue]() { enqueue([](service& s) { s.friends_open(); }); };
			command::add("steam_friends", [enqueue](const command::params& p) {
				if (p.size() == 2 && !std::strcmp(p[1], "scaleform"))
					enqueue([](service& s) { s.friends_open(true); });
				else if (p.size() == 1 && friends_menu_action) friends_menu_action();
			});
			command::add("steam_friends_refresh", [enqueue]() { enqueue([](service& s) { s.friends_refresh(); }); });
			command::add("steam_friends_page", [enqueue](const command::params& p) {
				if (p.size() != 2 || (std::strcmp(p[1], "next") && std::strcmp(p[1], "prev"))) return;
				const bool next = !std::strcmp(p[1], "next");
				enqueue([next](service& s) { s.friends_page(next); });
			});
			command::add("steam_friends_scroll", [enqueue](const command::params& p) {
				if (p.size() != 2) return;
				int first = 0; const std::string_view value(p[1]);
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), first);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || first < 0 || first > 256) return;
				enqueue([first](service& s) { s.friends_scroll(first); });
			});
			command::add("steam_friend_select", [enqueue](const command::params& p) {
				if (p.size() != 2) return;
				uint64 id = 0; const std::string_view value(p[1]);
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return;
				enqueue([id](service& s) { s.friends_select(id); });
			});
			command::add("steam_friend_invite", [enqueue]() { enqueue([](service& s) { s.friends_action(true); }); });
			command::add("steam_friend_join", [enqueue]() { enqueue([](service& s) { s.friends_action(false); }); });
			command::add("name", [enqueue](const command::params& p) {
				if (p.size() < 2)
				{ const auto* name = game::Dvar_FindVar("name"); console::info("name: %s\n", name && name->current.string ? name->current.string : "Player"); return; }
				const std::string desired = p.join(1);
				enqueue([desired](service& s) { s.set_name(desired); });
			});
			find_match_action = [enqueue]() { enqueue([](service& s) { s.find_match(); }); };
			// Native party cleanup also runs around frontend returns. It may
			// cancel a search, but must not retire a retained Steam match lobby.
			cancel_match_action = [enqueue]() { enqueue([](service& s) { s.cancel_find(false); }); };
			command::add("steam_find_match", []() { if (find_match_action) find_match_action(); });
			command::add("steam_match_cancel", [enqueue]() { enqueue([](service& s) { s.cancel_find(); }); });
			command::add("steam_match_start", [enqueue]() { enqueue([](service& s) { s.start_match(); }); });
			command::add("steam_match_vote", [enqueue](const command::params& p) {
				if (p.size() != 2 || (std::strcmp(p[1], "1") && std::strcmp(p[1], "2")))
				{ console::info("Usage: steam_match_vote <1|2>\n"); return; }
				const int choice = p[1][0] - '1';
				enqueue([choice](service& s) { s.vote(choice); });
			});
			command::add("steam_lobby_create", [enqueue](const command::params& p)
			{
				if (p.size() > 3 || (p.size() >= 2 && std::strcmp(p[1], "public") && std::strcmp(p[1], "friends")))
				{ console::info("Usage: steam_lobby_create [friends|public] [LAN IPv4:port]\n"); return; }
				const bool is_public = p.size() >= 2 && !std::strcmp(p[1], "public");
				const std::string endpoint = p.size() == 3 ? p[2] : "";
				enqueue([is_public, endpoint](service& s) { s.create(is_public, endpoint); });
			});
			command::add("steam_lobby_list", [enqueue]() { enqueue([](service& s) { s.list(); }); });
			command::add("steam_browser_refresh", [enqueue]() { enqueue([](service& s) { s.browser_refresh(); }); });
			command::add("steam_browser_filter", [enqueue]() { enqueue([](service& s) { s.browser_filter(); }); });
			command::add("steam_browser_join", [enqueue](const command::params& p) {
				uint64 revision = 0; unsigned int index = 0;
				if (p.size() != 3) { console::info("Usage: steam_browser_join <revision> <row>\n"); return; }
				const std::string_view r(p[1]), i(p[2]);
				const auto a = std::from_chars(r.data(), r.data() + r.size(), revision);
				const auto b = std::from_chars(i.data(), i.data() + i.size(), index);
				if (a.ec != std::errc{} || a.ptr != r.data() + r.size() || b.ec != std::errc{} || b.ptr != i.data() + i.size()) return;
				enqueue([revision, index](service& s) { s.browser_join(revision, index); });
			});
			command::add("steam_lobby_join", [enqueue](const command::params& p)
			{
				if (p.size() != 2) { console::info("Usage: steam_lobby_join <decimal lobby SteamID>\n"); return; }
				const std::string id = p[1];
				enqueue([id](service& s) { s.join(id); });
			});
			command::add("steam_lobby_leave", [enqueue]() { enqueue([](service& s) { s.leave(); }); });
			command::add("steam_lobby_invite", []() { if (friends_menu_action) friends_menu_action(); });
			command::add("steam_overlay_invite", [enqueue]() { enqueue([](service& s) { s.invite(); }); });
			scheduler::schedule([weak]()
			{
				const auto state = weak.lock();
				if (!state) return scheduler::cond_end;
				std::lock_guard lock(state->mutex);
				state->pump();
				return scheduler::cond_continue;
			}, scheduler::main);
			// Always initialize on the main frame, not during DLL attachment.
			scheduler::once([]() {
				game::Dvar_SetFromStringByName("csl_friends_scaleform", "0");
				game::Dvar_SetFromStringByName("csl_friends_status", "Loading Steam friends...");
				game::Dvar_SetFromStringByName("csl_friends_selected", "SELECT A FRIEND");
				game::Dvar_SetFromStringByName("csl_friends_count", "0");
				game::Dvar_SetFromStringByName("csl_friends_first", "0");
				for (int i = 0; i < 8; ++i)
				{
					game::Dvar_SetFromStringByName(("csl_friends_friend" + std::to_string(i)).c_str(), "");
					game::Dvar_SetFromStringByName(("csl_friends_id" + std::to_string(i)).c_str(), "");
				}
				if (!game::Dvar_FindVar("ui_browserShowNonDedicated"))
					dvars::Dvar_RegisterBool("ui_browserShowNonDedicated", 1, "Include player-hosted/listen servers in the browser; 0 shows dedicated servers only.", game::dvar_flags::saved);
				for (int i = 0; i < 50; ++i)
					game::Dvar_SetFromStringByName(("csl_browser_row" + std::to_string(i)).c_str(), "");
				game::Dvar_SetFromStringByName("csl_browser_count", "0");
				if (!game::Dvar_FindVar("csl_public_match"))
					dvars::Dvar_RegisterBool("csl_public_match", 0, "Internal Steam public-match progression policy; not a transport flag.", 0);
				if (!game::Dvar_FindVar("steam_useSteamName"))
					dvars::Dvar_RegisterBool("steam_useSteamName", 1, "Use the Steam persona in lobbies and freeze it for each match.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("steam_presenceStatus"))
					dvars::Dvar_RegisterBool("steam_presenceStatus", 1, "Append lobby/map status to the Project Consolation Companion activity title.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("sv_autostart"))
					dvars::Dvar_RegisterBool("sv_autostart", 0, "Start automatically after native minimum-player, lobby-time and countdown requirements.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("steam_autoLobby"))
					dvars::Dvar_RegisterBool("steam_autoLobby", 1, "Automatically advertise hosted matches through Steam.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("steam_hostAddress"))
					dvars::Dvar_RegisterString("steam_hostAddress", "", "Legacy LAN address preference; Steam SDR gameplay does not use this setting.", game::dvar_flags::saved);
			}, scheduler::main);
			enqueue([](service& s) { s.initialize(); });
#else
			friends_menu_action = []() {
				menu_file::set_friends_item("status", "Steam support is unavailable in this build.");
				menu_file::show_friends(true);
			};
			find_match_action = []() {
				scheduler::once([]() { find_match_error("Steam support is unavailable in this build. Offline and System Link play remain available."); }, scheduler::main);
			};
			cancel_match_action = {};
			scheduler::once([]()
			{
				console::warn("Steam online services unavailable in this build. To access online services, you must be signed into Steam and use a Steam-enabled client. Offline play remains available.\n");
			}, scheduler::main);
			for (const auto* name : { "steam_init", "steam_status", "steam_lobby_create", "steam_lobby_list",
					"steam_lobby_join", "steam_lobby_leave", "steam_lobby_invite", "steam_find_match", "steam_match_cancel", "steam_match_start", "steam_match_vote", "steam_presence_status", "steam_presence_retry", "steam_transport_status", "steam_browser_refresh", "steam_browser_filter", "steam_browser_join", "steam_friends_refresh", "steam_friends_page", "steam_friends_scroll", "steam_friend_select", "steam_friend_invite", "steam_friend_join", "steam_overlay_invite" })
				command::add(name, []() { console::info("Steam support not compiled; regenerate with --steamworks-sdk=<sdk root>.\n"); });
#endif
			command::add("steam_friends_close", []() { menu_file::show_friends(false); });
			command::add("xshowfriendslist", []() { if (friends_menu_action) friends_menu_action(); });
#ifndef CSL_STEAMWORKS
			command::add("steam_friends", []() { if (friends_menu_action) friends_menu_action(); });
#endif
			// QoS xshowfriendslist (103105E0): push 0; call XShowFriendsUI;
			// test EAX. Replace that command callback, not GFWL's system UI ABI.
			const auto friends_entry = game::game_offset(0x103105E0);
			const unsigned char friends_prefix[] = {0x6a,0x00,0xe8};
			const unsigned char friends_tail[] = {0x85,0xc0};
			if (!std::memcmp(reinterpret_cast<const void*>(friends_entry), friends_prefix, sizeof(friends_prefix))
				&& !std::memcmp(reinterpret_cast<const void*>(friends_entry + 7), friends_tail, sizeof(friends_tail))
				&& friends_entry + 7 + *reinterpret_cast<const std::int32_t*>(friends_entry + 3) == game::game_offset(0x10150ADA))
				friends_menu_hook.create(friends_entry, friends_menu_stub);
			else console::warn("Friends: unsupported native callback; use steam_friends.\n");
			// QoS PC 1.1 registers xpartygo -> 103102C0 (no args, caller
			// observes EAX). StartServer 102E08C0 queues map, not xpartygo.
			// Guarded compatibility hook: retain the original command outside
			// Steam playlist lobbies; remove when a native party bridge replaces it.
			const auto party_go = game::game_offset(0x103102C0);
			const auto party_go_global = static_cast<unsigned int>(game::game_offset(0x115D91D0));
			constexpr unsigned char party_go_tail[] = {0x80, 0x78, 0x10, 0x00, 0x75, 0x26};
			if (*reinterpret_cast<const unsigned char*>(party_go) == 0xA1
				&& !std::memcmp(reinterpret_cast<const void*>(party_go + 1), &party_go_global, sizeof(party_go_global))
				&& !std::memcmp(reinterpret_cast<const void*>(party_go + 5), party_go_tail, sizeof(party_go_tail)))
			{
				try { party_go_hook.create(party_go, party_go_stub); }
				catch (const std::exception& error) { console::error("Steam xpartygo routing failed: %s\n", error.what()); }
			}
			else console::error("Steam xpartygo: unsupported native command bytes; original behavior retained.\n");
			// QoS 1.1: the Scaleform playlist confirmation executes selectplaylist,
			// whose native tail enqueues xstartparty/LIVE search. Redirect only that
			// boundary; leave runplaylistrules, System Link and LAN sessions native.
			const auto select = game::game_offset(0x10325190);
			const auto cancel = game::game_offset(0x10323000);
			constexpr unsigned char select_prefix[] = {0x51, 0x56, 0x57, 0x68};
			constexpr unsigned char select_call[] = {0xE8, 0xA3, 0x0E, 0xF5, 0xFF};
			constexpr unsigned char cancel_test[] = {0x00, 0x74, 0x50, 0x83, 0x3D};
			if (std::memcmp(reinterpret_cast<const void*>(select), select_prefix, sizeof(select_prefix))
				|| std::memcmp(reinterpret_cast<const void*>(select + 8), select_call, sizeof(select_call))
				|| std::memcmp(reinterpret_cast<const void*>(cancel + 6), cancel_test, sizeof(cancel_test)))
			{
				console::error("Steam Find Match: unsupported native command bytes; menu routing was not installed.\n");
				return;
			}
			try
			{
				select_playlist_hook.create(select, find_match_stub);
				leave_match_hook.create(cancel, leave_match_stub);
				console::info("Steam Find Match: native playlist-confirmation routing installed.\n");
			}
			catch (const std::exception& error)
			{
				select_playlist_hook.clear(); leave_match_hook.clear();
				console::error("Steam Find Match: menu routing failed: %s\n", error.what());
			}
		}

		void pre_destroy() override
		{
			friends_menu_hook.clear(); friends_menu_action = {};
			party_go_hook.clear(); party_go_action = {};
			select_playlist_hook.clear(); leave_match_hook.clear();
			find_match_action = {}; cancel_match_action = {};
#ifdef CSL_STEAMWORKS
			if (state_)
			{
				std::lock_guard lock(state_->mutex);
				state_->shutdown();
			}
			state_.reset();
#endif
		}

	private:
#ifdef CSL_STEAMWORKS
		std::shared_ptr<service> state_;
#endif
	};
}

REGISTER_COMPONENT(steam_matchmaking::component)
