#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "engine/console/command.hpp"
#include "engine/console/console.hpp"
#include "utils/scheduler.hpp"
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
// persistence or gameplay transport. No native game addresses are patched here.
namespace steam_matchmaking
{
#ifdef CSL_STEAMWORKS
	namespace
	{
		constexpr auto product = "project-consolation";
		constexpr auto protocol = "qos-pc-1.1-lobby-v1";

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
				console::info("Steam initialized for App ID %u. Lobby title: Project: Consolation.\n", SteamUtils()->GetAppID());
				check_sign_in();
				status();
			}

			void pump()
			{
				if (active_ && !stopped_)
				{
					SteamAPI_RunCallbacks();
					check_sign_in();
				}
			}

			void status()
			{
				if (!active_) { console::info("Steam inactive; offline play unchanged. Use steam_init.\n"); return; }
				console::info("Steam: app=%u user=%llu online=%d lobby=%llu pending=%d\n",
					SteamUtils()->GetAppID(), SteamUser()->GetSteamID().ConvertToUint64(),
					SteamUser()->BLoggedOn(), lobby_.ConvertToUint64(), pending_);
				console::info("Steam lobbies are discovery only; gameplay/relay connection is not wired yet.\n");
			}

			void create(const bool public_lobby)
			{
				if (!ready() || lobby_.IsValid()) return;
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
				if (begin(call)) joined_.Set(call, this, &service::joined);
			}

			void leave()
			{
				// Do not cancel an in-flight join/create: cancelling its call result
				// would hide a successful join and leak untracked lobby membership.
				if (pending_) { console::warn("Steam: wait for the pending lobby operation, then leave.\n"); return; }
				if (active_ && lobby_.IsValid()) SteamMatchmaking()->LeaveLobby(lobby_);
				lobby_ = CSteamID();
			}

			void invite()
			{
				if (active_ && lobby_.IsValid()) SteamFriends()->ActivateGameOverlayInviteDialog(lobby_);
				else console::warn("Steam: create or join a lobby first.\n");
			}

			void shutdown()
			{
				stopped_ = true;
				created_.Cancel(); listed_.Cancel(); joined_.Cancel();
				if (active_)
				{
					// SDK 1.65 CCallback::Unregister calls the DLL unconditionally,
					// even for an unregistered manual callback. steam.dmp captured
					// __delayLoadHelper2 from this path after missing-DLL startup.
					// Only initialized sessions have registered these callbacks.
					invites_.Unregister(); kicked_.Unregister();
					if (lobby_.IsValid()) SteamMatchmaking()->LeaveLobby(lobby_);
					SteamAPI_Shutdown();
				}
				active_ = false; pending_ = false; lobby_ = CSteamID();
			}

		private:
			bool active_ = false, pending_ = false, stopped_ = false;
			bool offline_warning_shown_ = false;
			HMODULE module_ = nullptr;
			CSteamID lobby_;
			CCallResult<service, LobbyCreated_t> created_;
			CCallResult<service, LobbyMatchList_t> listed_;
			CCallResult<service, LobbyEnter_t> joined_;
			CCallbackManual<service, GameLobbyJoinRequested_t> invites_;
			CCallbackManual<service, LobbyKicked_t> kicked_;

			void unavailable(const char* reason)
			{
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
				if (failed || result->m_eResult != k_EResultOK)
				{
					console::error("Steam: lobby creation failed (IO=%d result=%d).\n", failed, failed ? -1 : static_cast<int>(result->m_eResult));
					return;
				}
				lobby_ = CSteamID(result->m_ulSteamIDLobby);
				auto* matchmaking = SteamMatchmaking();
				// Fail closed if the discovery namespace cannot be published.
				if (!matchmaking->SetLobbyData(lobby_, "csl_product", product) ||
					!matchmaking->SetLobbyData(lobby_, "csl_protocol", protocol) ||
					!matchmaking->SetLobbyData(lobby_, "name", "Project: Consolation"))
				{
					leave();
					console::error("Steam: lobby metadata publication failed; left lobby.\n");
					return;
				}
				status();
			}

			void listed(LobbyMatchList_t* result, const bool failed)
			{
				pending_ = false;
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
				if (failed || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
				{
					console::error("Steam: lobby join failed (IO=%d response=%u).\n", failed, failed ? 0u : result->m_EChatRoomEnterResponse);
					return;
				}
				lobby_ = CSteamID(result->m_ulSteamIDLobby);
				if (std::strcmp(SteamMatchmaking()->GetLobbyData(lobby_, "csl_product"), product) ||
					std::strcmp(SteamMatchmaking()->GetLobbyData(lobby_, "csl_protocol"), protocol))
				{
					leave();
					console::error("Steam: incompatible lobby; left without connecting to gameplay.\n");
					return;
				}
				status();
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
			command::add("steam_lobby_create", [enqueue](const command::params& p)
			{
				if (p.size() > 2 || (p.size() == 2 && std::strcmp(p[1], "public") && std::strcmp(p[1], "friends")))
				{ console::info("Usage: steam_lobby_create [friends|public]\n"); return; }
				const bool is_public = p.size() == 2 && !std::strcmp(p[1], "public");
				enqueue([is_public](service& s) { s.create(is_public); });
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
			enqueue([](service& s) { s.initialize(); });
#else
			scheduler::once([]()
			{
				console::warn("Steam online services unavailable in this build. To access online services, you must be signed into Steam and use a Steam-enabled client. Offline play remains available.\n");
			}, scheduler::main);
			for (const auto* name : { "steam_init", "steam_status", "steam_lobby_create", "steam_lobby_list",
				"steam_lobby_join", "steam_lobby_leave", "steam_lobby_invite" })
				command::add(name, []() { console::info("Steam support not compiled; regenerate with --steamworks-sdk=<sdk root>.\n"); });
#endif
		}

		void pre_destroy() override
		{
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
