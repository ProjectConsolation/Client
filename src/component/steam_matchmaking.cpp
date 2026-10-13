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
// persistence or gameplay transport. Steam advertises a reachable host endpoint;
// connectIP performs the existing QoS session query/native handshake. No relay
// or Steam authentication is implied by lobby membership.
namespace steam_matchmaking
{
	namespace
	{
		utils::hook::detour select_playlist_hook, leave_match_hook;
		std::function<void()> find_match_action, cancel_match_action;

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
		constexpr auto protocol = "qos-pc-1.1-lobby-v1";

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

		bool parse_host_endpoint(const std::string& text, uint32& ip, uint16& port)
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

		uint32 local_game_ip()
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
				if (active_ && !stopped_)
				{
					SteamAPI_RunCallbacks();
					check_sign_in();
					if (finding_ && GetTickCount64() - find_started_ >= 20000)
						finish_find("Steam search timed out. Check your connection and try Find Match again.");
					if (GetTickCount64() - last_game_poll_ >= 1000)
					{
						last_game_poll_ = GetTickCount64();
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
				console::info("Steam lobby joins use direct LAN game connections; Steam relay is not implemented.\n");
				presence_.print_status();
				console::info("Steam Find Match: searching=%d waiting_host=%d playlist=%s\n", finding_, waiting_host_, find_name_.c_str());
				if (lobby_.IsValid())
					console::info("Steam lobby: %d/%d members.\n", SteamMatchmaking()->GetNumLobbyMembers(lobby_),
						SteamMatchmaking()->GetLobbyMemberLimit(lobby_));
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
				host_ip_ = 0; host_port_ = 0;
				const auto* configured = game::Dvar_FindVar("steam_hostAddress");
				if (configured && static_cast<unsigned char>(configured->type) != game::DVAR_TYPE_STRING)
				{ console::error("Steam: steam_hostAddress must be an IPv4:port string.\n"); return; }
				const std::string address = !endpoint.empty() ? endpoint
					: configured && configured->current.string ? configured->current.string : "";
				if (!address.empty() && !parse_host_endpoint(address, host_ip_, host_port_))
				{
					console::error("Steam: specify a reachable IPv4:port, not loopback or a command string.\n");
					return;
				}
				if (!host_ip_) host_ip_ = local_game_ip();
				if (!host_ip_)
				{
					console::error("Steam: no unique LAN address. Set steam_hostAddress to your reachable LAN IPv4:port.\n");
					return;
				}
				const auto call = SteamMatchmaking()->CreateLobby(public_lobby ? k_ELobbyTypePublic : k_ELobbyTypeFriendsOnly, 18);
				if (begin(call)) created_.Set(call, this, &service::created);
			}

			void list()
			{
				if (!ready()) return;
				auto* matchmaking = SteamMatchmaking();
				matchmaking->AddRequestLobbyListStringFilter("csl_product", product, k_ELobbyComparisonEqual);
				matchmaking->AddRequestLobbyListStringFilter("csl_protocol", protocol, k_ELobbyComparisonEqual);
				matchmaking->AddRequestLobbyListFilterSlotsAvailable(1);
				matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
				matchmaking->AddRequestLobbyListResultCountFilter(50);
				const auto call = matchmaking->RequestLobbyList();
				if (begin(call)) listed_.Set(call, this, &service::listed);
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
				cancel_find();
				// Do not cancel an in-flight join/create: cancelling its call result
				// would hide a successful join and leak untracked lobby membership.
				if (pending_) { console::warn("Steam: wait for the pending lobby operation, then leave.\n"); return; }
				if (active_ && lobby_.IsValid())
				{
					if (playlist_lobby_ && SteamMatchmaking()->GetLobbyOwner(lobby_) == SteamUser()->GetSteamID())
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
				host_resolve_at_ = 0; resolving_find_host_ = false;
				// Respect an explicit leave while the same server is running.
				suppress_auto_lobby_ = running_server();
				host_ip_ = published_ip_ = attempted_ip_ = 0;
				host_port_ = published_port_ = attempted_port_ = 0;
				published_ready_ = false;
				observed_round_.clear();
			}

			void invite()
			{
				if (active_ && lobby_.IsValid()) SteamFriends()->ActivateGameOverlayInviteDialog(lobby_);
				else console::warn("Steam: create or join a lobby first.\n");
			}

			void shutdown()
			{
				command::set_steam_identity(0);
				presence_.shutdown();
				stopped_ = true;
				finding_ = false; candidates_.clear();
				created_.Cancel(); listed_.Cancel(); joined_.Cancel();
				if (active_)
				{
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
			bool offline_warning_shown_ = false;
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
			CCallbackManual<service, GameLobbyJoinRequested_t> invites_;
			CCallbackManual<service, LobbyKicked_t> kicked_;
			CCallbackManual<service, LobbyGameCreated_t> game_created_;

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
				uint32 ip = 0; uint16 port = 0; CSteamID server;
				return lobby.IsValid() && lobby.IsLobby()
					&& matchmaking->GetLobbyOwner(lobby) != SteamUser()->GetSteamID()
					&& !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_product"), product)
					&& !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_protocol"), protocol)
					&& (!std::strcmp(matchmaking->GetLobbyData(lobby, "csl_game_ready"), "1")
						|| !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_host_state"), "starting")
						|| !std::strcmp(matchmaking->GetLobbyData(lobby, "csl_host_state"), "waiting"))
					&& matches_rotation(find_rotation_, matchmaking->GetLobbyData(lobby, "csl_map"),
						matchmaking->GetLobbyData(lobby, "csl_gametype"))
					&& matchmaking->GetLobbyGameServer(lobby, &ip, &port, &server) && valid_game_ip(ip) && port;
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
					finish_find("Could not create the Steam host lobby. Check Steam and steam_hostAddress.");
				}
			}

			bool autostart_enabled() const
			{
				const auto* setting = game::Dvar_FindVar("sv_autostart");
				return setting && setting->current.enabled;
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

			void update_lobby_screen()
			{
				if (!lobby_.IsValid()) { menu_file::set_match_lobby(false); return; }
				auto* mm = SteamMatchmaking();
				// Only playlist lobbies own the persistent match/vote screen.
				if (!*mm->GetLobbyData(lobby_, "csl_vote_round")) return;
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
					menu_file::set_match_lobby_text("vote" + suffix, clean(mm->GetLobbyData(lobby_, ("csl_vote_map" + suffix).c_str()))
						+ " / " + clean(mm->GetLobbyData(lobby_, ("csl_vote_mode" + suffix).c_str())) + "  (" + std::to_string(counts[i]) + " votes)");
				}
				const bool owner = mm->GetLobbyOwner(lobby_) == SteamUser()->GetSteamID();
				const std::string state = mm->GetLobbyData(lobby_, "csl_host_state");
				menu_file::set_match_lobby_text("state", state == "closed" ? "Host left - leave this lobby and use Find Match again"
					: state != "waiting" ? "Preparing match..."
					: owner ? "Vote, then START MATCH when ready" : "Vote - waiting for the host to start");
				for (int i = 0; i < 18; ++i)
				{
					std::string name;
					if (i < mm->GetNumLobbyMembers(lobby_))
					{
						const auto member = mm->GetLobbyMemberByIndex(lobby_, i);
						const auto* display = mm->GetLobbyMemberData(lobby_, member, "csl_display_name");
						name = clean(*display ? display : SteamFriends()->GetFriendPersonaName(member));
						if (member == mm->GetLobbyOwner(lobby_)) name += " (HOST)";
					}
					menu_file::set_match_lobby_text("member" + std::to_string(i), name);
				}
			}

			void launch_find_host(const bool manual = false)
			{
				if (!manual && !autostart_enabled())
				{
					if (!SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "waiting"))
					{ leave(); find_match_error("Could not publish the waiting lobby."); return; }
					finding_ = false; waiting_host_ = true;
					console::info("Steam: lobby waiting for players. Use steam_status to check membership and steam_match_start to start.\n");
					return;
				}
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
				// Use QoS' working local session/gameplay transport, independent of
				// which frontend menu initiated the Steam host. No System Link menu
				// navigation or fake session/lobby state is required.
				if (!SteamMatchmaking()->SetLobbyData(lobby_, "csl_map", current[host_entry_].map)
					|| !SteamMatchmaking()->SetLobbyData(lobby_, "csl_gametype", current[host_entry_].gametype)
					|| !SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "starting"))
				{ leave(); find_match_error("Could not publish match startup."); return; }
				finding_ = false; find_host_pending_ = false; waiting_host_ = false;
				update_identity(); // capture the latest lobby persona before freezing
				host_launch_started_ = GetTickCount64();
				update_identity(true);
				command::execute("xstopparty; xstopprivateparty; systemlink 1; onlinegame 0; splitscreen 0; xblive_rankedmatch 0; uiscript \"uiScript StartServer\"");
				console::info("Steam Find Match: native match startup requested for %s/%s.\n",
					current[host_entry_].map, current[host_entry_].gametype);
			}

			void maintain_auto_lobby()
			{
				// An explicit map/Start Match action also ends pre-game waiting.
				if (waiting_host_ && running_server()) waiting_host_ = false;
				if (waiting_host_ && autostart_enabled()) start_match();
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
					if (!playlist_lobby_) { leave(); return; }
					auto_match_started_ = false;
					if (!SteamMatchmaking()->SetLobbyData(lobby_, "csl_game_ready", "0") || !prepare_votes(true)
						|| !SteamMatchmaking()->SetLobbyData(lobby_, "csl_host_state", "waiting"))
					{ leave(); find_match_error("Could not prepare the next lobby vote."); return; }
					waiting_host_ = true; published_ready_ = false;
				}
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

			void publish_game()
			{
				if (!lobby_.IsValid() || !SteamUser()->BLoggedOn()
					|| SteamMatchmaking()->GetLobbyOwner(lobby_) != SteamUser()->GetSteamID()) return;
				auto* matchmaking = SteamMatchmaking();
				if (!running_server() || !xlive::is_local_shim_loaded())
				{
					if (!waiting_host_ && !host_launch_started_ && !host_resolve_at_ && !resolving_find_host_)
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
				const auto port = host_port_ ? host_port_ : ntohs(network_port);
				if (!valid_game_ip(host_ip_) || !port) return;
				if (published_ready_ && published_ip_ == host_ip_ && published_port_ == port) return;
				if (!matchmaking->SetLobbyData(lobby_, "csl_game_ready", "1")) return;
				matchmaking->SetLobbyGameServer(lobby_, host_ip_, port, k_steamIDNil);
				published_ip_ = host_ip_; published_port_ = port; published_ready_ = true;
				console::info("Steam: published running QoS host %u.%u.%u.%u:%u.\n",
					host_ip_ >> 24, (host_ip_ >> 16) & 255, (host_ip_ >> 8) & 255, host_ip_ & 255, port);
			}

			void connect_game()
			{
				if (!lobby_.IsValid() || !SteamUser()->BLoggedOn()) return;
				auto* matchmaking = SteamMatchmaking();
				if (matchmaking->GetLobbyOwner(lobby_) == SteamUser()->GetSteamID()) return;
				if (std::strcmp(matchmaking->GetLobbyData(lobby_, "csl_product"), product)
					|| std::strcmp(matchmaking->GetLobbyData(lobby_, "csl_protocol"), protocol)
					|| std::strcmp(matchmaking->GetLobbyData(lobby_, "csl_game_ready"), "1")) return;
				uint32 ip = 0; uint16 port = 0; CSteamID server;
				if (!matchmaking->GetLobbyGameServer(lobby_, &ip, &port, &server)
					|| !valid_game_ip(ip) || !port) return;
				// A new round can reuse the same UDP endpoint. Do not mistake it
				// for the connection already attempted in the previous round.
				const std::string round = matchmaking->GetLobbyData(lobby_, "csl_vote_round");
				if (round != observed_round_)
				{ observed_round_ = round; attempted_ip_ = 0; attempted_port_ = 0; }
				if (attempted_ip_ == ip && attempted_port_ == port) return;
				attempted_ip_ = ip; attempted_port_ = port;
				waiting_game_since_ = 0;
				if (running_server() || !xlive::is_local_shim_loaded())
				{
					console::warn("Steam: stop your own match and deploy the LAN xlive shim before joining; then leave/rejoin the lobby.\n");
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
				console::warn("Steam online services unavailable: %s\n", reason);
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
				else warn_sign_in();
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
				auto_lobby_ = auto_create_requested_; auto_create_requested_ = false;
				if (cancelled_auto_create_)
				{ cancelled_auto_create_ = false; leave(); return; }
				auto* matchmaking = SteamMatchmaking();
				// Fail closed if the discovery namespace cannot be published.
				if (!matchmaking->SetLobbyData(lobby_, "csl_product", product) ||
					!matchmaking->SetLobbyData(lobby_, "csl_protocol", protocol) ||
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
					matchmaking->SetLobbyGameServer(lobby_, host_ip_, host_port_ ? host_port_ : ntohs(network_port), k_steamIDNil);
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
			command::add("steam_lobby_join", [enqueue](const command::params& p)
			{
				if (p.size() != 2) { console::info("Usage: steam_lobby_join <decimal lobby SteamID>\n"); return; }
				const std::string id = p[1];
				enqueue([id](service& s) { s.join(id); });
			});
			command::add("steam_lobby_leave", [enqueue]() { enqueue([](service& s) { s.leave(); }); });
			command::add("steam_lobby_invite", [enqueue]() { enqueue([](service& s) { s.invite(); }); });
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
				if (!game::Dvar_FindVar("steam_useSteamName"))
					dvars::Dvar_RegisterBool("steam_useSteamName", 1, "Use the Steam persona in lobbies and freeze it for each match.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("steam_presenceStatus"))
					dvars::Dvar_RegisterBool("steam_presenceStatus", 1, "Append lobby/map status to the Project Consolation Companion activity title.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("sv_autostart"))
					dvars::Dvar_RegisterBool("sv_autostart", 0, "Automatically start a Find Match host instead of waiting in its Steam lobby.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("steam_autoLobby"))
					dvars::Dvar_RegisterBool("steam_autoLobby", 1, "Automatically advertise hosted matches through Steam.", game::dvar_flags::saved);
				if (!game::Dvar_FindVar("steam_hostAddress"))
					dvars::Dvar_RegisterString("steam_hostAddress", "", "Reachable IPv4:port override for automatic Steam hosting (empty selects the unique LAN address).", game::dvar_flags::saved);
			}, scheduler::main);
			enqueue([](service& s) { s.initialize(); });
#else
			find_match_action = []() {
				scheduler::once([]() { find_match_error("Steam support is unavailable in this build. Offline and System Link play remain available."); }, scheduler::main);
			};
			cancel_match_action = {};
			scheduler::once([]()
			{
				console::warn("Steam online services unavailable in this build. To access online services, you must be signed into Steam and use a Steam-enabled client. Offline play remains available.\n");
			}, scheduler::main);
			for (const auto* name : { "steam_init", "steam_status", "steam_lobby_create", "steam_lobby_list",
				"steam_lobby_join", "steam_lobby_leave", "steam_lobby_invite", "steam_find_match", "steam_match_cancel", "steam_match_start", "steam_match_vote", "steam_presence_status", "steam_presence_retry" })
				command::add(name, []() { console::info("Steam support not compiled; regenerate with --steamworks-sdk=<sdk root>.\n"); });
#endif
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
