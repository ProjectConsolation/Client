# Commands

## Viewmodel field of view and motion blur

- `cg_vm_fov <degrees>`: saved viewmodel-only FOV, `0` (default) retains stock behavior. Use `65`–`100` to adjust weapon framing without changing world FOV. Native ADS zoom is retained; valid nonzero range is 1–160 degrees.
- `r_blur_extra <0|1>`: saved toggle, default `0`. Disables the common motion-blur pass, including normal camera motion, falling, and cover dash. `1` restores the stock motion-blur rules. Flashbangs, damage effects, depth of field, and menu blur are unchanged.
- `cl_drawping <0|1>`: saved toggle, default `1`. Shows numeric ping in the Tab scoreboard; `0` restores the stock connection bars. Uses the native scoreboard ping value.

## In-game Steam friends panel

- `csl_clantag [open|apply|cancel]`: opens the four-character clan-tag input. Apply uses the same validation and profile persistence as `clanName`; Cancel/Escape discards edits. Available through **Clan Tag** in online, match, private-match and System Link setup menus. The reserved `CSL` tag remains restricted to the authorized Steam ID.
- **Start Game** appears first in the Steam match lobby only for its waiting host. It uses `steam_match_start`, which rechecks host ownership before starting; guests cannot launch a match.

The Friends/Party Invite entry and Steam match lobby's Invite Friends button
open a QoS-styled native friends panel. This adapts the sorted SteamID-based
selection/paging approach from KisakBlack's `live_friends_pc.cpp` and
`ui_friends.cpp`, not BO1 binary layouts or Xbox dashboard assets.

- `steam_friends` (also native `xshowfriendslist`): open the frontend/lobby panel.
- `steam_friends_close`: Back, preserving the owning menu and Steam lobby.
- `steam_friends_refresh`: refresh names/status; open panels also refresh once a second.
- `steam_friends_page <prev|next>`: page through eight friends at a time, online first.
- `steam_friend_select <SteamID>`: select a friend from the current snapshot.
- `steam_friend_invite`: invite the selected current friend to your existing lobby.
- `steam_friend_join`: join the selected friend's advertised lobby through the existing
  product/protocol/security checks. Leave your own lobby/match first; this does not
  silently abandon it. App ID 480 alone is never treated as product compatibility.
- `steam_lobby_invite`: now opens this panel; `steam_overlay_invite` retains Steam's
  overlay invitation picker as an explicit fallback.

Install `consolation/menu/ui_mp/steam_friends.menu` with the rebuilt client. The
panel currently opens in frontends/lobbies, not during gameplay/map loading.
Use keyboard, mouse, or the existing native-menu controller navigation. Offline
Steam shows a sign-in message; empty/removed friends cannot produce valid actions.
Friends use app-scoped `csl_lobby` presence so Companion title customization does
not become a dependency of friend joining. Names are display-only, not commands.
The stock Scaleform Party Invite and Invite Friends buttons explicitly execute
`steam_friends`. Install matching loose movies and `patch_consolation.ff`; a new
DLL alone does not supply a missing friends-menu asset. Stock party/match lists
consume the display-only `csl_roster_active`, `csl_roster_count` and
`csl_roster_name0` through `csl_roster_name17` values. Signed-in users appear as
the local member before queueing; Steam membership supplies names after joining.
Steam row indices never invoke native LIVE kick/gamercard actions.

Two-account acceptance: open Party Invite, page/select a friend, invite them,
then leave your own lobby and use Join Lobby. Verify native map entry, not just
Steam membership. Rename/remove a selected friend and refresh; selection must
remain on the same ID or clear, never move to another person. Back/Escape must
restore the prior menu and controller focus without leaving the lobby. Test
Steam signed out, no friends, unrelated Spacewar lobbies and both input methods.

## Network security diagnostics

- `networkSecurityStatus`: reports installation of the bounded native Huffman,
  fragment-capacity and member-join hooks, plus decode/join rejection counts.
  Expect `huffman=1 fragments=1 join=1`. `netchanIncomplete` includes ordinary
  intermediate fragments and is not an attack count.
- `securityGuardSelfTest`: existing malformed-join/directive smoke tests. A pass
  does not certify valid joins, memory safety of the entire engine or absence of RCE.
- `offlineNetStatus`: now also reports `steam_guard=1` once the hardened Steam
  ingress policy activates. The `rejected` count includes discarded raw UDP.

Both `d3d9.dll` and `xlive.dll` must be rebuilt and deployed together. Steam
lobbies use `qos-pc-1.1-lobby-v3-auth`; older relay clients are incompatible.
Authenticated Steam member metadata binds each member's native XNADDR/XUID;
bindings cannot change during the lobby and duplicate identities are rejected.
The native join's claimed XUID must match its authenticated inbound route.
This preserves local profile XUIDs rather than replacing them with Steam IDs.

While Steam ingress is active, native reception accepts only registered loopback
bridge endpoints and their bound identities. Direct LAN UDP is intentionally
blocked, including after leaving/signing out of Steam. Restart with Steam closed
for the separate offline/LAN mode; that plaintext LAN mode is not authenticated.
See [the two-PC acceptance guide](../../required_files/PLAYTEST_STEAM_PLAYLISTS.txt).

## Local QoS playlists

The adapted `playlists.info` preserves the dumped QoS stock playlists 1–9,
including their localized text and weighted map rotations. Stock modes and Gun
Game set `scr_player_respawndelay` to zero. CTF definitions, if present in the
source, retain their existing delay; CTF is absent from the current retail dump.
Playlist 10 adds Gun Game (`gg`) on Conflict's stock rotation. Playlist 11 is
`[DEV] Shipment 24/7`, rotating Gun Game and Conflict exclusively
on `mp_shipment`. It is selectable but excluded from random Quick Match.

`tools/package_playlists.py` adapts an original dump without transcoding its
localized bytes, preserves existing patch rawfiles, and bundles the playlist
and the existing Gun Game script under its canonical `gg.gsc` name. Install the
resulting `patch_consolation.ff` and rebuild the client for the local acquisition
adapter. With the offline LIVE shim, playlist acquisition invokes QoS's native
parser and reports ready only after nonempty native playlist tables exist.
The local adapter waits for the unique `consolation/playlists.info` rawfile;
parsing the stock asset before the patch zone loads would permanently cache
only the retail nine playlists. Both canonical and namespaced copies are bundled.
This replaces the earlier success-only download probe; it does not supply a
remote playlist server. Find Match uses the loaded native rotation to filter
Steam hosts (see the Steam section below).
The shim does not advertise its own private party as a discovered remote host:
native session search is empty until a real discovery provider is connected.
With no XHV voice engine, the native join headset query reports no headset
instead of dereferencing null. This is not an implementation of voice chat.

## Steam lobby integration (development)

Generate the solution with `generate.bat --steamworks-sdk="C:\path\to\sdk"`
or set `STEAMWORKS_SDK` to that SDK root. Build the client yourself. Deploy the
SDK's **32-bit** `redistributable_bin/steam_api.dll` beside `JB_LiveEngine_s.exe`;
it is also copied into the build output. Nightly packaging requires this x86
redistributable, includes it at the ZIP root beside the client, and verifies its
packaged hash. Missing or wrong-architecture redistributables fail packaging.
Steam initialization runs by default
on the game's main frame; no `-steam` switch is required. Delay-loading keeps
a missing Steam redistributable from preventing offline startup. If Steam is
unavailable or the user is not signed in, the client warns that online services
require signing into Steam and continues with offline play. A sign-out after
initialization produces the same warning once, not every frame. Failed SDK
initialization retries automatically every five seconds, with repeated identical
errors suppressed. Run `steam_init` to request an immediate attempt after
starting/signing into Steam. A build generated
without SDK support also warns and retains offline play.

Initialization supplies the configured build-time App ID automatically when no
`SteamAppId`/`SteamGameId` launch environment or `steam_appid.txt` in the working
directory exists. It sets only the current process environment, never overwrites
the file, and prints the selected default. `--steam-app-id=<ID>` selects a different
authorized ID during solution generation. The current default **480** is explicitly
a Spacewar development probe, not a finished release identity. Its Steam title
is Spacewar, not Consolation. No manual `steam_init` or App ID file is needed for
this default test startup; initialization still requires Steam to be running.
App 10080 belongs to Quantum of Solace, not this project. The lobby's `name`
metadata is **Project: Consolation**; this does not rename Steam's app title.
The separate Companion adapter publishes the title **Project Consolation**.
It does not rename the storefront App ID or change matchmaking's App ID.

### Steam display names and developer tag

`steam_useSteamName` defaults to `1` (saved). The local offline-shim display
name follows your Steam persona in the lobby; connecting/loading and active
matches freeze that name until returning to the lobby. A Steam rename during
a match applies to the next match, not the current game. Late SDK initialization
retains the already-active name. QoS' LIVE name cache accepts 15 UTF-8 bytes;
names are truncated at a complete character and userinfo/control syntax is removed.
XUID and signed-in profile identity are reserved before display changes and are
not replaced by a Steam ID or derived again from your new display name.

`name <display name>` selects a custom name and disables `steam_useSteamName`.
During a match that custom name is queued for the next lobby/match. `name` with
no arguments prints the current display name. Use `seta steam_useSteamName 1`
to resume Steam naming. This adapter requires the local shim; real GFWL
signed-in identities are not rewritten. Direct `set name` is protected in-game.

The `CSL` clan tag is never automatically applied. Use `clanName CSL` manually;
it is reserved for the configured owner Steam account and requires the initialized
SDK to report that account signed in. Debug and Release builds use the same rule.
Other accounts and signed-out Steam are denied, including case/color/whitespace
variants; native UI/profile/`seta clanName` routes are also checked. Bots no longer
receive the reserved tag automatically.

The server userinfo guard strips `CSL` from unverified remote clients; only the
authorized owner's local loopback client is currently exempt. **Even the owner's
remote connection is denied until Steam authentication tickets are bound to QoS
connections.** A peer-supplied Steam ID or lobby membership does not prove who
owns a gameplay connection. This is fail-closed tag filtering, not completed
Steam gameplay authentication or an authorization role. Modified/older servers
without the guard cannot be secured by this client-side policy.

### Companion custom Steam activity (Windows)

The client implements the documented Companion Windows IPC protocol v1/build 2.
It publishes a per-game registration and automatically runs the official
`companion_attach` entry through hidden native 64-bit `rundll32.exe` when a
compatible `companion64.dll` is present beside `JB_LiveEngine_s.exe`. It never
loads that x64 DLL into the x86 game. Startup is asynchronous and nonfatal.
Attachment is prepared before SDK initialization so Companion can capture the
first activity report. Startup polls without blocking a frame and proceeds
without custom presence if attachment is unavailable or takes over 20 seconds.
The module modifies Steam itself and remains resident until Steam exits; game
shutdown withdraws only this game's registration, not the Steam hook.
The custom title is applied to activity reports sent to friends. Your own Steam
Friends panel can still display the SDK App ID's title (Spacewar for 480); verify
the custom activity from another account rather than using that local label.

- `steam_presence_status`: print registration, actual module state/build/hook,
  Steam PID and requested title. State `1` means active; registration alone
  does not prove friends see the title.
- `steam_presence_retry`: retry attachment after supplying the missing DLL.
  A resident unsupported/failed module cannot be replaced without manually
  restarting Steam. The client does not restart Steam for you.

`steam_presenceStatus` defaults to `1` (saved), adding a short activity suffix:
menus, searching, lobby member count, or the active map/mode. Set it to `0` for
exactly `Project Consolation`. Companion supports changing this title text, not
a separate rich-presence panel or a gameplay/relay implementation. Text fits its
64-byte UTF-8 record; only adapter-generated ASCII status is published.

The Companion DLL is an external dependency, not built by this project. No
Windows release asset was available from its official repository at integration
time. The adapter is present but custom activity is unavailable until a compatible
module is supplied. Distribute the external module with its license notices and
corresponding source information; the adapter does not link its library code.
Protocol-layout declarations are attributed to the
[IW4x Companion manual](https://github.com/iw4x/companion/blob/main/libcompanion/doc/manual.cli).

- `steam_init`: retry SDK initialization; print a nonfatal warning on failure.
- `steam_status`: show actual App ID, Steam user, online state and current lobby.
- `steam_find_match`: same search as confirming a playlist in **Find Match**.
  Search public Steam hosts whose current/planned map/mode is an entry in the
  selected native playlist, then join a compatible lobby and query its Steam
  endpoint. If none exists, reserve a public lobby, select an entry with QoS'
  native weighted rotation and wait for the host to start (or start automatically
  when `sv_autostart` is `1`). Native playlist rules are applied before waiting.
- `steam_match_start`: start the selected match from your waiting Find Match
  lobby. Host-only; pending Steam operations must complete first. The higher
  voted option wins; ties retain the native weighted first option.
- `xpartygo`: in a Steam playlist lobby, the host can manually start the winning
  map/mode, like `steam_match_start`. Non-hosts cannot launch it. Outside Steam
  playlist lobbies, the original private-match/System Link command is retained.
- `steam_match_vote <1|2>`: vote for one of the two map/mode options in the
  current Steam match lobby. One changeable vote per current member; ballots
  from departed members or previous rounds are not counted.
- `steam_match_cancel`: cancel Find Match without stopping a manually hosted
  lobby. Leaving the native match-search screen also cancels the search.
- `steam_lobby_create [friends|public]`: create an 18-member
  lobby, friends-only by default. Public lobbies appear in searches. No LAN
  address or port forwarding is required by the Steam transport. The native
  match publishes its actual shim bind port for datagram routing.
- `steam_lobby_list`: search worldwide for available Consolation protocol-v2
  lobbies, excluding unrelated games sharing a development App ID.
- `steam_browser_refresh`: populate the shared System Link Join Game / online
  Server Browser with compatible **running** Steam hosts, independent of the
  selected playlist. Full lobbies are shown but cannot be joined. Waiting
  pregame lobbies remain Find Match results, not active-server rows.
- `steam_browser_join <revision> <row>`: join a zero-based row from the current
  browser snapshot. The UI passes its revision so stale refresh results cannot
  silently join a different host. Rechecks compatibility and capacity.
- `steam_lobby_join <decimal lobby SteamID>`: join and check protocol metadata;
  incompatible lobbies are immediately left. Wait for the host to start a
  match, establish the Steam connection, query its local bridge and enter the
  native QoS connection handshake. A failed query does not retry forever;
  fix connectivity, leave the lobby and rejoin to retry.
- `steam_lobby_leave`: leave the current lobby. Wait for pending create/join/list
  completion first so an asynchronous join cannot create untracked membership.
- `steam_lobby_invite`: open the native Steam friends panel for the current lobby.
  `steam_overlay_invite` opens the Steam overlay picker instead.
- `steam_transport_status`: print packet counters and each peer's connection
  state/relay POP. Connected is state `3`; a nonzero relay POP confirms relay
  routing, not completion of the native game handshake.

Gameplay datagrams now use **Steam Networking Messages / SDR**, via loopback-only
UDP bridges. Both peers need matching Steam-enabled clients and the offline
`xlive.dll` shim. Public host IPs and router forwarding are no longer part of
the Steam join path. Cross-network gameplay still requires a two-PC runtime
playtest; local transport tests alone cannot establish that it works in-game.
No System Link navigation or
lobby commands are needed for discovery: use **Find Match** to join or create a
lobby. With the default `sv_autostart 0`, the host uses `steam_match_start` when
ready; `steam_status` shows the lobby member count. This is Steam lobby membership,
not a native QoS party roster. The original QoS Scaleform match lobby shows Steam names,
two vote options and totals, and Start Match/Invite Friends/Leave Lobby actions.
The old custom match-lobby panel is not loaded or packaged in nightly builds.
Normal round completion retains the Steam lobby; Back/Leave Lobby explicitly leaves it. Starting
a hosted game through other menus or a map command also creates a public Steam
lobby automatically. The manual `steam_lobby_create` command remains a diagnostic
operation and does not itself launch a server.
Gun Game matches `gg` entries; `[DEV] Shipment 24/7` accepts only
`mp_shipment/gg` and `mp_shipment/dm`. Hosts publish their current map/mode.
Search/reservation requests are bounded to 20 seconds and preserve cancellation
across late Steam join/create callbacks. Reserved starting hosts are discoverable;
joining peers remain in deliberately waiting lobbies without a startup timeout.
Once startup begins they wait up to 60 seconds for the actual running endpoint.
A five-second reservation visibility window and follow-up search prefer an
existing running match or a lower visible Steam owner before launching another
host. This is best-effort lobby coordination, not an authoritative distributed
matchmaker; simultaneous starts require a two-account runtime test.
Finding a host does not prove the subsequent QoS handshake succeeded; verify
both players enter the map.

The online party lobby has **Server Browser** immediately below **Find Match**.
It uses the existing System Link join screen and controls; Back returns to
the online party lobby without destroying that party. The System Link entry
uses the same active Steam-host listing when Steam relay support is available;
otherwise the original native browser remains in place. Steam discovery is
not a replacement LAN discovery provider for unsigned-in offline peers.
The browser's occupancy currently represents Steam lobby members, not a
separately queried native connected-player count. F5 refreshes the list.

Hosts advertise `csl_server_kind=listen` or `dedicated`. Find Match accepts only
explicit listen hosts; the browser accepts either kind. Unknown/old unclassified
advertisements are rejected. This is a separation boundary, **not** a completed
headless dedicated-server directory: a future server provider must publish real
availability, player counts and a supported connection endpoint. No fabricated
dedicated listings are added.

`cslMenuRoute browseronline` opens the shared browser from online play;
`browserlocal` marks a System Link entry without switching screens; `browserback`
returns to its owning menu. These are frontend integration routes.

`steam_autoLobby` defaults to `1` and advertises manually started native hosts;
set it to `0` to disable that implicit advertisement. Explicit Find Match still
creates a host when necessary. `steam_hostAddress` is a legacy LAN setting and
is no longer used to route Steam gameplay or override the actual bind port.
Find Match lobbies persist when the match stops: members return to the voting
screen, retaining the lobby ID. The host prepares another pair of playlist
map/mode options. With `sv_autostart 0`, Start Match launches the winning option;
`sv_autostart 1` waits for the playlist's minimum players, minimum lobby time,
pregame delay and visible countdown; voting stays open until launch. Dropping
below minimum players, editing settings or disabling autostart cancels the
countdown. Gameplay still uses the native
datagram serialization, but its System Link return page no longer owns Steam lobby routing.
Host migration after the host leaves is not implemented; remaining members must
leave and find/create another lobby. Diagnostic/manual-map auto-advertisements,
which have no playlist rotation, are still left when the match stops.
Explicitly leaving a lobby
during a running match suppresses recreation until that match ends.

`sv_autostart` is a saved boolean preference, default `0`. Set
`seta sv_autostart 1` to enable automatic starts, including subsequent rounds.
The host reads QoS' `party_minplayers`, `party_maxplayers`,
`party_minLobbyTime`, `party_pregameStartTimerLength` and
`party_gameStartTimerLength`, rather than imposing a new fixed countdown.
The Steam lobby screen receives the host's timer; Steam members, not a synthetic
native party roster, provide its player count. Native team assignment and the
gameplay handshake remain separate; the original LIVE party/team-readiness
state machine is not restored by this adapter. Use
`seta sv_autostart 0` to wait for a manual `steam_match_start`. It does not gate
explicit System Link Start Match actions or `map`/`devmap` commands.

Find Match currently supports solo joining, not moving a multi-player party.
It will not silently leave an existing Steam lobby or stop your own match.
Old hosts without map/mode metadata are not eligible; rebuild both peers.

Lobby operations and callbacks run on the main pipeline. XUIDs/profiles stay
unchanged. Steam authenticates transport peer identities, and the bridge admits
only current lobby members. Native QoS client/userinfo authorization (including
the reserved clan tag) is still separate and not yet bound to those identities.
Accepted overlay invites follow the same validated lobby/game handoff without
silently leaving an existing lobby. Only locally allocated bridge IP/port fields
are passed to `connectIP`, never command text from lobby metadata. Test on two
Steam accounts with matching App IDs; validate failure/offline behavior separately.

References: [Steamworks API](https://partner.steamgames.com/doc/sdk/api),
[matchmaking and lobbies](https://partner.steamgames.com/doc/features/multiplayer/matchmaking),
[Steam Networking Messages](https://partner.steamgames.com/doc/api/ISteamNetworkingMessages).

## Scaleform frontend recovery

`sf_restart` resumes QoS's saved native frontend movie play state and requests
the multiplayer main menu. Use it from the console if returning from a match
or disconnect leaves an empty/unresponsive frontend. Close the console afterward.
It clears pending custom menu routes and does not automatically reopen System Link.

The command is restricted to a disconnected frontend with a ready native movie
and no running local server. It does not disconnect a match, restart the renderer,
recreate the GFx heap, or change profiles/session settings. This is a manual recovery
adapter, not a confirmed repair of the transition bug; test recovery with a rebuilt
client DLL. Neither command reconstructs the ActionScript loader or restores an
initial-state snapshot; the native resume operation only restores play/pause state.

The frontend transition repair is separate from this recovery command. Native
`startInAnimASAP` now retains a pending destination when its movie has not loaded;
`ScreenLoader.onLoadInit` completes that handoff only for the current destination
after the outgoing animation ends. Missing cache entries no longer count as loaded.
The initial screen and explicit suppressed-Out requests own their handoff directly:
there is no outgoing timeline to signal completion. Already-cached screens start
immediately; asynchronous screens retain their destination for `onLoadInit`.
Superseded navigation cannot activate an old screen. System Link return waits for
the main menu's native `Screen` resting frame, then requests `mpsyssetup`, the
System Link Create Game screen with Start Match, Select Map, Select Game Mode
and Match Options. Opening that screen refreshes map/mode fields; it does not
activate Start Match. PC `mpsyslink` is only the Create/Join Option popup, not
the lobby and not a separate `mpsyslink.swf`. Requesting it as a movie plays the
main menu's Out animation and leaves an empty destination marked `Loading`.
Navigation to the same screen name is skipped only when that screen is visible,
interactive and has completed its incoming animation—not when a hidden or
deactivated clip merely retains the requested name.

Deploy the updated client DLL and synchronized embedded/loose frontend movies.
Do not assume a script-only `patch_consolation.ff` contains repaired movies:
inspect its rawfile inventory before relying on it as a frontend override.
`tools/patch_scaleform_transitions.py` updates all embedded class copies without
replacing menu settings or controller art. `tools/package_scaleform_patch.py`
preserves existing patch rawfiles when adding the repaired movie bundle.
If movies are embedded in `common_consolation.ff`, refresh that zone with
`tools/package_controller_resources.py` after changing the loose release bundle.
Its embedded rawfiles must match `required_files/consolation/scaleform` byte for
byte. Either source may resolve and cache first during startup, so updating only
the embedded zone or only the loose files can leave an old movie resident even
with a current DLL. Synchronize the installed `consolation/scaleform` directory
as well as the zone, then fully restart the game to discard cached movies.
`tools/test_build_common_consolation.py` checks this packaging invariant.
Use `--initial-load-only` with the transition patcher to upgrade a bundle that
already contains the previous asynchronous handoff fixes.
Verify ordinary match completion, manual disconnect, and repeated host/join
cycles without invoking `sf_restart`. Compilation/read-back and event-order
tests do not prove that every GFx runtime return path is repaired.

`sf_status` prints the resident frontend's current/destination screen, pending
load, Out-animation flag, readiness, visibility, controller activation and frame
numbers. It also reports the System Link loader cache entry and destination clip
name, loaded frames, timeline position and controller activation, distinguishing
a pending load from an initialized clip that never became interactive.
It is read-only and does not restart the movie. Use its output when
reporting a frontend that freezes at the end of an animation.

## Killcam and flash HUD behavior

The packaged killcam script places the countdown 12 virtual pixels lower and
shows `Killed by NAME` above it, resolving the actual attacker entity. The label
is a live, non-archived foreground element and is destroyed with the other
killcam elements. The native damage-feedback hitmarker remains archived, but
uses the foreground pass so recorded killer hits remain visible while the
ordinary gameplay HUD is hidden. Chat retains its existing replay drawing path.
The marker uses a zero-position center/middle origin rather than signed
half-size offsets, keeping its center on the crosshair in archived snapshots.

The DLL suppresses the native crosshair/friendly-name drawing pass during the
same flash state tested by QoS's `Flashbanged()` menu expression. It does not
disable all HUD elements, alter shellshock duration, or change spectator names.
The guard reads native game time as a scalar, including valid time zero, and
only examines screen-blend parameters while the effect is active.
Install the matching `patch_consolation.ff`, rebuild the DLL, and restart to
validate replay hits, label cleanup, and name suppression/recovery after flashes.

## Direct IP connections

`connectIP <IPv4>:<port>` (for example `connectIP 127.0.0.1:1000`) queries the endpoint for its QoS session information,
then invokes the native session/party connection path. The default UDP port is
1000 when the port is omitted. The old separate-port syntax remains compatible.
`connectIP cancel` cancels a pending query; `connect_direct` remains a
compatibility alias. Replies must match the endpoint, challenge and Consolation
protocol, and include valid session fields and slot counts. A query times out
after five seconds.

Consolation's replacement `xlive.dll` is required for this local LAN transport;
the client detects it automatically without `-offline` or `-offline_test`.
The command checks that the shim exposes a nonzero signed-in XUID before
querying. It does not create a separate GUID or claim peer authentication. Use
normal persistent identities on the two machines. The shim creates random
XUIDs under `%LOCALAPPDATA%\ConsolationClient\offline-identities`, independently
of names, Discord, or hardware IDs. Identical player names no longer derive the
same XUID. Do not copy identity files between peers or include them in packages.
Keep matching client/shim versions and let the host start a multiplayer match
before connecting. Both session join and actual gameplay still require a
two-machine test. This command does not implement NAT traversal; the advertised
UDP endpoint must be reachable through the firewall/router.

### Ordinary two-PC nightly play

Install the matching optional shim in both game roots and launch normally with
`Launch Consolation.bat`. Host through SYSTEM LINK, then join using its browser
or `connectIP <host-IP>:1000`. Both PCs can use the default UDP 1000/1001 because
their IPs differ. The shim remains optional for the client for now, but highly
recommended and required for Consolation's local GFWL emulation/LAN transport.
Debug flags and `offlineNetStatus` are not setup requirements. Player renames
preserve the selected local identity/profile. Back up identity files together
with game `storage`; first use retains the previous profile directory key.

## Text chat and killcam HUD

The default chat keys are **T** for all-chat (`chatmodepublic`) and **Y** for
team-chat (`chatmodeteam`). The old T/team + Y/public pair is migrated after
config loading; custom and unbound keys are left alone. To explicitly select
these bindings, use `bind t chatmodepublic` and `bind y chatmodeteam`.

During killcam, native chat messages remain visible while normal gameplay HUD
is suppressed. The match clock uses the live, non-foreground HUD pass rather
than a killer's archived clock. The killcam's own countdown remains visible.
Verify both receiving chat during replay and restoration of the normal clock
after respawning; source/contract tests are not an in-game rendering test.

### Developer-only isolated two-process test

Rebuild and deploy both matching DLLs yourself. The replacement `xlive.dll` is
still required; Microsoft's system runtime is not. Start the host with:

```text
JB_Launcher_s.exe -multiplayer -offline -offline_test -offline_test_port=1000 -name TestHost
```

Start the playable client with `-offline_test_port=1002 -name TestClient` and
the same other switches. QoS opens a gameplay socket and an adjacent discovery
socket, so reserve 1000/1001 for the host and 1002/1003 for the second process.
`-offline_test_port=<1..65534>` overrides these binds before native networking
starts; it has no effect without `-offline_test`. Do not use `+set net_port` as
a substitute: launcher command forwarding happens after initial socket setup.

In both consoles, run `offlineNetStatus`. It prints test-mode state, process ID,
XUID, bound gameplay port (`0` means not bound), peer count, envelope traffic
counters and isolated storage path, followed by native dedicated/server-running
and session-handle/session-active state. The client-owned diagnostic contains no
session keys. `tools/prepare_offline_connection_test.ps1 -GameDirectory <path>
-Role Host` (or `Client`) checks required files and prints launch instructions
and DLL hashes without launching anything or changing firewall rules.

Create/start a match through the host's SYSTEM LINK / CUSTOM GAME menu, without
bots. On the second instance use `connectIP 127.0.0.1:1000`. For two computers,
use the host's LAN IPv4 address instead and allow the selected UDP ports on a
trusted private network. Foreground each window in turn to control its player.
Verify distinct XUIDs, two roster entries, movement visible to the other player,
fire/damage/death/respawn, disconnect and rejoin. Repeat on two machines with
matching DLL hashes. Keep both console logs; a query reply or increasing packet
counters is not sufficient proof of authoritative gameplay.

Test identities include machine name, process ID and creation time; each launch
gets separate shim profile/title storage under `storage/debug-net/<run>/` and
does not import a real GFWL profile. This isolation does **not** include native
engine graphics/key-binding config files. Console logs use separate
`console-offline-test-<pid>-<tick>.log` filenames in the usual log directories.
Use separate game copies if configs must also be isolated. Ordinary offline identities
and storage are unchanged when the flag is absent.

The local-shim adapter preserves engine datagrams inside a versioned envelope and
keeps advertised peer XNADDR/session records for reverse lookup instead of
fabricating remote identities from the local user. It supports at most 64 peer
records per XNet lifetime. It is plaintext trusted-LAN test infrastructure, not
XNet encryption, authenticated matchmaking, Internet P2P or NAT traversal.
Both peers need matching replacement shims. The adapter also runs for ordinary
offline profiles; only ephemeral identities/storage and bind overrides require
`-offline_test`. Direct queries include QoS's native two-byte VDP header and a
reply route, so the native hard-coded port 1000 does not lose replies to an
ephemeral query socket. Native SYSTEM LINK discovery broadcasts carry the same
identity envelope; replies route to the client's gameplay port, not its adjacent
discovery socket. Refresh SYSTEM LINK on the same private LAN after starting a
host match. Discovery uses UDP broadcast port 1001, so keep the host base port
1000 for this test. Custom host ports require direct IP connection.

### Experimental offline dedicated host

Add `-dedicated -set g_gametype dm -map mp_shipment` to the host launch above.
The client first completes normal initialization, then asks QoS's native
dedicated-latch transition to shut down local client processing before forwarding
map commands. It enables the native System Link session gate and suppresses the
map-start local session-membership call. Native session creation/completion and
server gameplay remain authoritative. Missing prerequisites refuse map startup.

This is **not a renderer-free cold-start server**: graphics/resources still
initialize briefly, and the game window is then hidden. It still requires the
full game installation. Rebuild both DLLs and prove this path at runtime before
using it as a dedicated server: `dedicated=1`, `sv_running=1`, native session
handle/active both `1`, no controllable host/host roster entry, and remote joins,
movement, combat, disconnect/rejoin. A second playable debug instance must omit
`-dedicated`. Do not force native session-active bytes to obtain a reply.

QoS's dedicated `infoResponse` changes only the `hw` field; it uses the same
session fields as a playable host. The actual response gate is an active native
session, not a different dedicated response format. The query/discovery and
dedicated changes are source-verified, not yet two-machine gameplay-verified.

## Discord Rich Presence

`discordRpcStatus` reports the local connection and activity acknowledgement.
See [Discord Rich Presence](Discord-Rich-Presence) for setup and saved dvars.

Project: Consolation includes a small set of utility and debugging commands that are useful for advanced users, modders, and troubleshooting.

## Commands

### Controller prompts

`controller_status` reports connected devices, active input source and the resolved controller icon family. The archived string dvar `ui_controllerIcons` accepts `auto` (default), `xbox`, or `ps3`. Auto selects Xbox for XInput and PS3 for other supported controller families; explicit values override the artwork without changing bindings. Unknown values use auto. Use `seta ui_controllerIcons ps3` for a PlayStation controller exposed through an XInput wrapper, or `seta ui_controllerIcons auto` to restore detection.

Native binding prompts use live key bindings and switch back to PC binding text on keyboard input, mouse clicks/wheel or foreground mouse movement, regardless of the artwork override. PC activity also disables controller aiming immediately. Returning requires neutral sticks/triggers, released PC keys/buttons, at least 750 ms without PC activity, and a fresh controller gesture; continuous stick movement, repeats and reconnects cannot bypass it. Install the updated bundled `consolation/zone/common_consolation.ff` and restart; absent materials fall back to readable button names. No loose controller PNGs are needed at runtime. See [Controller Support](ControllerSupport) for installation and validation details.

### Renderer timing

`gfxperf start [frames]` starts an opt-in CPU wall-clock capture of successful
native swaps (default `600`, range `30` to `4096`). It stops automatically at
the sample limit, or after that many failed presents. `gfxperf stop`,
`gfxperf report`, or bare `gfxperf` stops capture and prints the retained results.
Starting again clears the previous capture. No files are written.

The report includes mean, median, p95, p99 and maximum milliseconds for:

- Swap-end intervals: cadence including rendering, simulation, caps and waits.
- Native swap phase: includes QoS's focus/critical-section wait and Present.
- Inside Present: CPU time in the swap-chain call, not GPU execution duration.
- Swap outside Present: native phase overhead/waits outside that call.

The first interval and intervals above 250 ms are excluded from cadence stats
to avoid counting debugger pauses or map-load gaps; long swap durations remain
visible. Failed presents are counted separately and break the cadence chain.
Zero sample counts mean no valid timing data for that metric. Settings printed
at report time are context only, not a history of changes during the capture.

For comparison, keep map, camera, bots, resolution, AA, focus, frame cap and
VSync unchanged. Capture with the console closed, then reopen it to report.
A long Present can reflect VSync or queued GPU work; these timings alone do
not prove CPU/GPU saturation or predict a D3D9Ex speedup. Profiling is off by
default and performs no GPU queries, forced flushes, or resource/device changes.
The QoS PC 1.1 patch sites are guarded; this requires an updated client and
in-game validation.

`gfxinfo` prints the D3D9 AA request, pending latched request, actual sample
type (`0` means disabled), quality index, native depth format, AF settings and
hardware/effective filtering limits. It lists sample counts supported by both
the color and depth formats in the current window mode. It changes no settings.
This command requires the updated client; capability queries do not establish
that a mode has passed in-game testing.

| Name | Description | Example |
| :--- | :--- | :--- |
| `addbot [count]` | Spawns one or more multiplayer bots and assigns names from `consolation/bots.txt` when available. | `addbot` `addbot 4` |
| `dvarDump [filename]` | Prints all registered dvars to the console and can optionally write them to a text file under the `consolation` folder. | `dvarDump` `dvarDump dvars` |
| `commandDump [filename]` | Prints all registered command names to the console and can optionally write them to a text file under the `consolation` folder. | `commandDump` `commandDump commands` |
| `listassetpool <poolnumber> [filter]` | Lists assets from the selected asset pool and can optionally filter the output by text. | `listassetpool 0` `listassetpool 13 weapon` |
| `origin [0|1]` | Debug builds only. Toggles a twice-per-second origin and velocity status line in the internal game console; an explicit value enables or disables it. | `origin` `origin 1` `origin 0` |
| `cslMenuRoute <options\|host\|join\|online\|private\|loadout\|stats\|map\|mode\|rules\|main\|browseronline\|browserlocal\|browserback\|leave\|discord>` | Frontend routing: `browseronline` opens the shared join screen; `browserback` returns to the online party lobby when entered there, otherwise LOCAL PLAY. `map`/`mode` open custom pickers, `loadout`/`stats` retain native screens, and `leave` performs native party cleanup. `discord` opens the community invite. | `cslMenuRoute browseronline` |
| `openmenu <name>` | Opens a parsed disk menu when available, otherwise uses native asset lookup. | `openmenu csl_main` |
| `reloadMenus` | Rescans disk definitions. Refuses to replace menus still in the native open stack. | `reloadMenus` |
| `bind <key> "<command>"` | Uses native key binding with the stock-action whitelist removed. Command case, quoting, key-up handling and config persistence remain native. | `bind F5 "noclip"` |

## Notes

- If you provide a filename to `dvarDump` or `commandDump`, `.txt` is added automatically if needed.
- `commandDump` is still rough and may produce mangled or incomplete results in some cases.
- `listassetpool` can also be run without arguments to print the available pool numbers and asset type names.

## Camera

`cg_thirdPerson` retains its native cheat protection and default, but accepts integer modes `0` through `2`:

- `0`: native first-person selection.
- `1`: native QoS third-person/body-camera behavior.
- `2`: classic IW3-style trailing camera, using QoS's retained distance, angle and collision-trace implementation rather than the body-bone camera. Adjust `cg_thirdPersonRange` and `cg_thirdPersonAngle` as usual.

Mode `2` is implemented in source but still requires a rebuild and in-game testing. Check camera collision near walls, stance transitions, switching all three modes, and map restart. It is not an independent free camera or a complete replacement of QoS's special cameras.

## Movement

These QoS PC 1.1 hooks require rebuilding the client and in-game validation. Their patch sites are checked before installation; a mismatched executable logs a warning and skips the relevant patch.

| Dvar | Default | Behavior |
| :--- | :--- | :--- |
| `pm_movement_mode` | `stock` | Server-replicated enum: `stock` restores native behavior for the seven movement controls below; `iw3` selects the experimental COD4-style preset. Applied on preset changes by the host/frontend, not repeatedly over custom tuning or remote server settings. |
| `pm_adsStopsSprint` | `0` | Server-replicated. ADS ends sprint through the native sprint-ending routine; sprint remains suppressed for the remainder of the movement step. `0` restores stock behavior. |
| `pm_adsExitOnDamage` | `1` | Server-replicated. `0` suppresses native scoped-ADS cancellation on damage, including per-player overrides. `1` permits the original policy; it does not force cancellation. Damage, recoil, flinch and script `allowads` restrictions are unchanged. |
| `pm_allowProne` | `2` | Server-replicated integer: `0` rejects new prone permission checks, `1` allows native prone despite the weapon gate, `2` restores stock weapon permission. Clearance, ladder and script restrictions remain native. Does not forcibly stand an already-prone player. Animation and prediction require testing. |
| `pm_allowCover` | `1` | Server-replicated. `0` blocks native cover entry, including automatic/queued entry, clears the entry hint, and begins a native forced exit when already in cover. Native exit animation and cleanup continue normally. `1` permits the original rules; it does not override stock `cover_disable` or script restrictions. |
| `pm_mantleFirstPerson` | `0` | Server-replicated experimental camera option for native `mantle_40`, `mantle_44` and `mantle_56` traversals. Uses the native first-person view branch and suppresses traversal/model camera replacement. `0` restores the stock camera. Does not port COD4 mantle movement or add COD4 hand animations. |
| `pm_climbFirstPerson` | `0` | Server-replicated experimental camera option for native ladders, ledges and pipes. Same camera-only behavior as the mantle option; balance, generic transitions and wallhug retain their stock camera. Explicit `cg_thirdPerson` requests and death/spectator views are preserved. |
| `pm_airborneBobScale` | `1` | Server-replicated float, range `0`–`1`. Scales the native movement camera bob speed input while airborne and outside traversal. `0` suppresses that bob; `1` restores stock amplitude. Grounded bob, velocity, recoil, landing effects and damage kick are not changed. This is a targeted mitigation; the reported excessive gun movement still needs reproduction and verification. |

Use `pm_movement_mode iw3` for sprint-to-ADS, retained ADS on damage, prone permission, disabled cover, first-person mantle/climb cameras and suppressed airborne bob. Use `pm_movement_mode stock` to restore all seven native policies. Individual controls remain adjustable after applying a preset; selecting a different preset replaces those adjustments. Re-entering the same preset does not reapply it. This does not reset unrelated jump/speed settings.

QoS already contains jump-step collision logic matching the KisakCOD algorithm, including the native 18-unit jump step. The preset preserves that path; broader ledge/strafe and traversal parity is not yet verified or a complete COD4 physics port.

For example, `bind Z "+prone"` uses the native prone input command. Remote prediction requires matching client hooks and server-replicated settings; mixed stock/patched clients have not been validated. All movement additions above are source implementations pending a rebuild and runtime tests, not verified gameplay fixes.

After rebuilding, test sprint-to-ADS with keyboard and controller; prone with and without overhead clearance; cover entry and toggling off while already in cover; each mantle height, ladder, ledge and pipe in both camera modes; and jump/crouch/strafe with `pm_airborneBobScale 0` versus `1`. Repeat after respawn and a map change, and on a patched client/server pair. Watch for `[movement] ... skipped` warnings indicating an unsupported patch site.

## Game Settings: FOV and FPS

Game-mode selector labels use the loaded language's native StringEd translations
for retail, custom/title-storage and appended mode rows. Quoted or `@`-prefixed
references are normalized before lookup; literal custom names remain literal.
This is a client code fix and requires rebuilding/restarting the DLL, not
replacing gametype scripts in `patch_consolation.ff`.

Options → Gameplay (Game Settings) includes native **FOV** and **MAXIMUM FPS** sliders with visible numeric values. FOV selects 65–160 degrees in steps of 5. Maximum FPS selects 30 (NATIVE), 60, 85, 125, 250, 333, or 0 (UNLIMITED). FPS help warns that going over 333 FPS may break physics. Mouse dragging, keyboard arrows and controller navigation use the stock slider handling.

**HORIZONTAL SAFE AREA** and **VERTICAL SAFE AREA** use the same native slider
handling with integer percentages from 0 to 100 in steps of 5 (default 85).
They control `safeArea_horizontal` and `safeArea_vertical`; console values remain
fractions (for example, `safeArea_horizontal 0.85`). Both are saved and apply to
the in-game HUD without changing front-end menus or overlays.

Both controls apply immediately and use the saved, non-cheat `cg_fov` and `com_maxfps` dvars; no video restart is needed. Graphics retains its FPS control. Console values outside these presets are not rewritten merely by opening Game Settings; adjusting a slider selects a preset.

Ship both `common_consolation.ff` (menu movies/controller art) and `patch_consolation.ff` (gameplay scripts) under `consolation/zone/`. The script zone contains namespaced adaptations of retail respawn, killcam, match-clock and System Link money scripts, plus authored ambience source/placement inputs for all 12 retail MP maps. Namespaced lookup prevents later map-zone loading from replacing fixes. Explicit loose gametype overrides still take priority. Ambience retains runtime sound-alias validation rather than blindly enabling all CreateFX content. Install the matching client DLL and restart; regenerating assets alone does not update the lookup hook.

`tools/package_patch_consolation.py` packages scripts from the local retail zone directory through the standalone `tools/patch_gameplay_scripts.cpp` asset transform utility. It rejects unsupported script anchors and existing output paths, and verifies the compressed fastfile by reading it back. Nightly packaging requires and hash-checks both zones; ordinary local builds do not generate a nightly.

## Native video options

The patched stock video menu uses the native Apply and Keep/Restore confirmation flow. It adds Display mode (Fullscreen, Fullscreen Windowed, Windowed), Maximum FPS presets (30 Native, 60, 85, 125, 250, 333, 0 Unlimited), and 1x/2x/4x/8x/16x anisotropic filtering. Selecting 0 displays a warning that going over 333 FPS may break physics. V-Sync retains its stock toggle. AA requests are Off/2x/4x/8x/16x; unsupported extended requests fall back to a supported color-and-depth sample mode. `gfxinfo` reports the actual result, which can differ from the requested menu value.

Display mode uses the existing `r_fullscreen` and `r_borderless` dvars: `1/0` is exclusive fullscreen, `0/1` is fullscreen windowed, and `0/0` is bordered windowed. Fullscreen windowed uses the complete monitor nearest `vid_xpos`/`vid_ypos`, including the taskbar area. Scene and display buffers use the monitor resolution; the resolution selector applies to the other two modes. For lower-end hardware, a smaller bordered window may be cheaper than desktop-resolution borderless mode. A console `com_maxfps` or filtering value outside the menu presets is retained as a Custom entry. Opening the menu does not change it. AF controls both `r_texFilterAnisoMin` and `r_texFilterAnisoMax`; native material policy and hardware clamping are retained.

Install all nine bundled native menu files under `consolation/scaleform/`, preserving rollback copies, then fully restart the game. These are `cmfrontend.gfx`, `MpMainMenu.gfx`, `cmsharedplatform.gfx`, `pcsharedlibrary.gfx`, `CmOptions.gfx`, `cmoptionsgraphics.gfx`, `cmaftervidrestart.gfx`, `CmOptCntlr.gfx` and `mpcustomloadout.gfx`. The frontend bootstrap constructs the menus before the first screen loads, so the bootstrap and companion files must be installed together. The graphics layout moves the settings divider and help down while preserving the native bottom separator, title and footer. The release copies live under `required_files/consolation/scaleform/`; nightly packaging requires all nine and checks their archived hashes. DLL-only post-build deployment remains unchanged. No D3D9Ex backend or performance-default changes are enabled by this patch.

The video row is labeled V-Sync and uses native `r_vsync` apply/revert behavior. QoS PC registers this single VSync control at `0x103AF210`; the presentation parameters routine at `0x103BCFB0` reads its boolean to select synchronized or immediate presentation. `sf_revertvalues_r_vsync` is native confirmation rollback storage, not a second renderer control. No nonexistent VSync aliases are registered. Apply changes to restart the renderer. The native Escape footer reads QUIT on the multiplayer main menu and BACK elsewhere. Submenus go back, while the multiplayer root opens the native Yes/No quit confirmation without immediately exiting. Set Optimal Settings has native separators aligned with its row edges; APPLY and the help text have separate space, and the bottom footer separator remains intact.

Leaving Graphics with pending changes opens the native **EXIT WITHOUT SAVING?** Yes/No popup. No or Escape returns to Graphics with pending selections intact; Yes discards those selections and returns to the previous menu without applying settings or restarting the renderer. Leaving with no pending changes goes back directly. The existing Apply and post-restart Keep/Restore confirmations remain unchanged.

The PLAY ONLINE help text is “Play online multiplayer.” The shared button-list Escape dispatcher intercepts the main menu before its native empty-stack guard, opening the existing quit confirmation instead of ignoring Escape. Other screens retain their native Back behavior. Archive verification does not replace an in-game check after restart.

The Discord help text reads “Join the Discord for Project: Consolation updates!” Controls includes a Controller tab immediately below Multiplayer, using the native Controls screen. Its ordering is adapted from [IW4x's gamepad menu](https://github.com/iw4x/iw4x-rawfiles/blob/main/iw4x/iw4x_00/ui_mp/pc_options_gamepad.menu), with this project's actual dvars: enablement (`gpad_enabled`), inversion (`input_invertPitch`), stick layout (`gpad_sticksConfig`), button layout (`gpad_buttonsConfig`), sensitivity level (`gpad_sensitivity`), sensitivity multiplier (`input_viewSensitivity`), inner/outer stick deadzones, and menu stick threshold (`gpad_stick_pressed`). Changes take effect without a renderer restart. Selecting a button preset reapplies controller bindings through `bindgpbuttonsconfigs`; it does not replace keyboard/mouse bindings. Default/Tactical and their Alt variants are exposed; selecting Tactical swaps melee/stance, while Alt swaps grenade shoulder buttons. Stick presets are Default, Southpaw, Legacy, and Legacy Southpaw. Opening the tab does not reset custom bindings. Feedback and cheat-protected aim settings are not exposed as working user options.

New setting labels and values use uppercase, including CONTROLLER and V-SYNC. The decorative P99 plane is hidden only in the Controls movie; binding rows, native panels and other screens are preserved.

Native LOADOUT now opens the class editor directly. A shopping-bag STORE tab labeled 0 sits to the left of class 1. Click it, or cycle left from class 1 with Page Up / `[` and confirm with Enter, to open the native store. Store navigation is separate from the six real profile classes; it never writes class 0. Returning from the store restores the class editor. Existing equipment selectors, purchase rules and profile storage remain native. Verify mouse, keyboard and controller navigation, purchases, cancellation and persistence after restarting.

After rebuilding and restarting, test all three modes, Alt-Tab, repeat Apply/Keep, reject a change with No and with the timeout, reopen video options, and relaunch with saved settings. Compare `gfxinfo` before/after AA and AF changes. Test both fullscreen and windowed sample support; do not assume a menu request proves the device accepted it. These source changes require runtime validation.

## Native Main Menu

The stock GUIDE help text is “Join the Discord for Project: Consolation updates!” and Windows LIVE is labeled PLAY ONLINE. The stock-movie patch removes Single Player and the visible Quit Game entry. Escape at the multiplayer root opens the existing native Yes/No quit popup; cancellation returns to the underlying menu and Yes retains the stock quit action. Options and lobby back navigation are unchanged. Install the complete bundled native menu set described above, keeping rollback copies, and fully restart the game. These files modify the stock movies; they do not install the experimental `.menu` frontend.

Stock Scaleform menus are used when the custom `.menu` definitions and main-menu `.gfx` override are not installed. The stock GUIDE item is relabeled DISCORD and its native `xshowguideui` command opens the community invite in the default browser. Other stock labels, actions and layout are preserved. This source change requires a rebuilt client and restart; already-loaded movies remain cached. To temporarily restore stock menus, move the custom assets outside the game directory and keep a rollback copy—no opt-in switch is required. Avoid reinstalling the custom assets when testing the stock menu.

DLL post-build deployment no longer copies experimental menu assets automatically. Regenerate existing Visual Studio projects after this deployment change so their old post-build steps cannot reinstall those assets. Custom menu sources remain available for manual installation later.

Development implementation; the matching client must be rebuilt and the menu files installed before testing.

`consolation/menu/ui_mp/main.menu` defines five horizontal navigation tabs and two play cards. ONLINE PLAY opens the custom matchmaking/private-match hub; LOCAL PLAY opens the custom create/join hub. Campaign has been removed. DISCORD opens the community invite in the default browser. The right-aligned logo has 65% opacity; native black fills at 35% opacity dim the background video and preview images without a black PNG.

The focus/action/dialog pattern follows the [COD4 Mod Tools main menu](https://github.com/promod/CoD4-Mod-Tools/blob/master/raw/ui_mp/main.menu). Syntax references: [OpenAssetTools](https://openassettools.dev/asset/menu/structure.html) and [Quake III menu guide](https://icculus.org/~phaethon/q3tamenu/q3tamenu-1.html). Definitions are expanded rather than importing unsupported macros or expressions.

Slideshow backgrounds select `consolation/images/preview_mp_*.png` (or `preview_sp_*.png` for the corresponding special background). They preserve aspect ratio, move only right within a fixed inset preview rectangle, advance every eight seconds, and crossfade for 1.5 seconds. Labels and controls are painted above the preview. Missing images leave the panel intact.

`lobbies.menu` supplies seven custom pages: online/local hubs, private/System Link lobbies, settings, map selection and game-mode selection. Online Play retains Create a Class, statistics, invites and private-match actions. Player panels use the native party-name feeder (`25`) inside a bordered list, with QoS's native signed-in-player fallback. This is not a fabricated roster or a replacement online service. Session initialization, sign-in and match ownership remain native.

Map selection uses QoS feeder `1`; game modes use feeder `29`. Their native selection handler updates `ui_mapname`/`ui_mapname_text` and `ui_gametype`/`ui_gametype_text`, respectively. The ABI field is a float at item offset `376`, not an integer; listbox union data also requires `dataType == 6`. `execKeyInt` supplies native Enter/keypad-Enter handlers so moving through a list does not accidentally confirm or join. Matchmaking playlist selection, loadouts, statistics and advanced rules retain their native screens. The selected-map lobby background resolves `preview_mp_<ui_mapname>.png` and makes a slow, eased two-minute right/left round trip; the main-menu card slides remain right-only. A 35% black fill dims both without an opaque bitmap.

Mode display files use their first token as the localization reference; subsequent fields such as `team` and `maxPlayers` are not part of the label. Labels resolve through the loaded game language. The internal `MENU` mode is visible only with a Debug client DLL, including after native UI list reloads. Release clients filter it by ID without hiding the last playable mode. Feeder count, text and selection bounds use the same full list.

Button actions play `ui_pause_select`, focus changes play `ui_pause_updown`, and Escape plays `ui_pause_back` through QoS's native menu `play` command. These aliases match the shipped PC Scaleform shared-library actions rather than names borrowed from another game.

`sf_enable` is zero while a managed custom screen is active so the original background video can remain visible. Native destination screens temporarily re-enable Scaleform; returning reopens the corresponding custom root or host screen. Escape at the main menu opens quit confirmation. Escape in a hub returns to the root; leaving an online host lobby also uses native party cleanup. `cslMenuRoute main` is a recovery route, while `leave` performs online lobby cleanup first.

Custom frontend pages close when the client begins connecting/loading and stay closed during gameplay, even if `sf_current_menu` still reports the previous lobby. The native return-to-main-menu transition enables the custom frontend again. Loading screens, HUDs and native pause menus are not replaced by this lifecycle guard.

Native decorations do not take button focus. PNG materials use private copies of the engine's 2D pipeline. `reloadMenus` rescans definitions only after they are closed. The parser, crop/fade timing, menu layout, Escape behavior, LIVE sign-in, host/guest permissions, return transitions, and display scaling require verification with the rebuilt client. Never treat a successful screen transition alone as proof of a valid party or match.

The optional legacy disk browser `csl_serverbrowser` uses native feeder `2` and
the custom LOCAL PLAY return route. The current Scaleform `mpsysjoin` browser
retains its original four-column layout and consumes the Steam browser snapshot
when available; see the Steam section above. Its online entry has its own return
route. Do not confuse legacy native discovery with the new Steam provider.

Loadouts opened from the System Link host (including its settings tab) go directly to QoS's native `mpcustomloadout` class editor using local sign-in, rather than the LIVE-gated `mploadout` shop wrapper. Online and private-online lobbies retain the original LIVE route. This does not introduce separate loadout storage or unlock equipment; native profile ownership still applies. Returning to the frontend restores the originating custom lobby. This routing change requires a rebuilt DLL and in-game validation.

## Launch Commands

### Temporary audio tracing (Debug DLL only)

`snd_trace start [seconds]` records a bounded, read-only trace of native sound
resumes, ends, pauses, and channel state after `SND_Update`. The default is five
seconds; accepted durations are 1–15 seconds. Close the console and fire/move
while it records. It automatically saves a uniquely named
`consolation/sound-trace-<pid>-<tick>.csv`. `snd_trace stop` stops early and saves
the current records. Capture separately on a silent map and a working map.

The trace records sample status, byte position, volume, native callsite, pause/
mute state, and nonzero values in Miles' software mixer buffer. Mixer values
are not proof of audible output. Resume events precede native channel alias
registration, so their alias field may be empty or refer to the previous voice;
subsequent frame events provide the registered alias. The trace does not alter
audio state, substitute samples, or apply map-specific behavior. Hooks are
installed only on first use, limited to the verified QoS 1.1/Miles build, and
removed during shutdown. Background-thread events are excluded. The capture
is limited to 4096 records and loaded 2D/3D channels, not streamed voices.
This is a temporary investigation tool, not a sound repair, and requires a
rebuilt Debug DLL. Release DLLs do not register it.

### In-game video menu adapter

`cslIngameVideo <action>` is the native pause menu's staged-video adapter.
Normal use is **Pause → GRAPHICS**, not manual console invocation. Supported
actions are `begin`, `mode`, `quality`, `display`, `aa`, `aniso`, `vsync`,
`shadows`, `lights`, `textures`, `models`, `marks`, `position`, `apply`, `exit`,
and `discard`. `begin` snapshots the current values; adjustments remain
pending until `apply` sets the dvars and requests a native renderer restart.
`position` stages `vid_xpos=0` / `vid_ypos=0`; it also requires Apply.
`exit` opens an Exit Without Saving confirmation when pending values differ,
and `discard` leaves engine values unchanged.

### System Link balance and local profiles

With the local XLive shim installed, offline mode retains QoS's native `mpdata` loading and session-end saving through the shim's local title storage. The System Link script adaptation permits the existing money award/read path; other ranked-only statistics remain ranked-only. It does not create an online leaderboard service or bypass Store purchase rules.

After rebuilding, verify this with two machines: note each player's starting balance, earn money in a System Link match, finish the match normally, then restart and check that each balance persists independently. Source/script regression tests are not proof that the full runtime session and storage transitions work.

Commands passed to `JB_Launcher_s.exe` with either a `+` or `-` prefix are forwarded to the multiplayer engine command buffer. Launcher-only switches such as `-multiplayer` and `-offline` are consumed by Consolation instead of being submitted as game commands.

```text
JB_Launcher_s.exe -multiplayer -offline -name mac-dev1 -seta com_maxfps 125 +devmap mp_barge
```
