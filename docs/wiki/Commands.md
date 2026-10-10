# Commands

## Discord Rich Presence

`discordRpcStatus` reports the local connection and activity acknowledgement.
See [Discord Rich Presence](Discord-Rich-Presence) for setup and saved dvars.

Project: Consolation includes a small set of utility and debugging commands that are useful for advanced users, modders, and troubleshooting.

## Commands

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
| `cslMenuRoute <options\|host\|join\|online\|private\|loadout\|stats\|map\|mode\|rules\|main\|browserback\|leave\|discord>` | Frontend routing: `map`/`mode` open custom pickers, `loadout`/`stats` retain native screens, `browserback` returns to LOCAL PLAY, and `leave` performs native party cleanup before returning to the appropriate hub. `discord` opens the community invite. Does not emulate online services. | `cslMenuRoute options` |
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

## Native Main Menu

Stock Scaleform menus are used when the custom `.menu` definitions and main-menu `.gfx` override are not installed. The stock GUIDE item is relabeled DISCORD and its native `xshowguideui` command opens the community invite in the default browser. Other stock labels, actions and layout are preserved. This source change requires a rebuilt client and restart; already-loaded movies remain cached. To temporarily restore stock menus, move the custom assets outside the game directory and keep a rollback copy—no opt-in switch is required. Avoid reinstalling the custom assets when testing the stock menu.

Development implementation; the matching client must be rebuilt and the menu files installed before testing.

`consolation/menu/ui_mp/main.menu` defines five horizontal navigation tabs and two play cards. ONLINE PLAY opens the custom matchmaking/private-match hub; LOCAL PLAY opens the custom create/join hub. Campaign has been removed. DISCORD opens the community invite in the default browser. The right-aligned logo has 65% opacity; native black fills at 35% opacity dim the background video and preview images without a black PNG.

The focus/action/dialog pattern follows the [COD4 Mod Tools main menu](https://github.com/promod/CoD4-Mod-Tools/blob/master/raw/ui_mp/main.menu). Syntax references: [OpenAssetTools](https://openassettools.dev/asset/menu/structure.html) and [Quake III menu guide](https://icculus.org/~phaethon/q3tamenu/q3tamenu-1.html). Definitions are expanded rather than importing unsupported macros or expressions.

Slideshow backgrounds select `consolation/images/preview_mp_*.png` (or `preview_sp_*.png` for the corresponding special background). They preserve aspect ratio, move only right within a fixed inset preview rectangle, advance every eight seconds, and crossfade for 1.5 seconds. Labels and controls are painted above the preview. Missing images leave the panel intact.

`lobbies.menu` supplies seven custom pages: online/local hubs, private/System Link lobbies, settings, map selection and game-mode selection. Online Play retains Create a Class, statistics, invites and private-match actions. Player panels use the native party-name feeder (`25`) inside a bordered list, with QoS's native signed-in-player fallback. This is not a fabricated roster or a replacement online service. Session initialization, sign-in and match ownership remain native.

Map selection uses QoS feeder `1`; game modes use feeder `29`. Their native selection handler updates `ui_mapname`/`ui_mapname_text` and `ui_gametype`/`ui_gametype_text`, respectively. The ABI field is a float at item offset `376`, not an integer; listbox union data also requires `dataType == 6`. `execKeyInt` supplies native Enter/keypad-Enter handlers so moving through a list does not accidentally confirm or join. Matchmaking playlist selection, loadouts, statistics and advanced rules retain their native screens. The selected-map lobby background resolves `preview_mp_<ui_mapname>.png` and makes a slow, eased two-minute right/left round trip; the main-menu card slides remain right-only. A 35% black fill dims both without an opaque bitmap.

Button actions play `ui_pause_select`, focus changes play `ui_pause_updown`, and Escape plays `ui_pause_back` through QoS's native menu `play` command. These aliases match the shipped PC Scaleform shared-library actions rather than names borrowed from another game.

`sf_enable` is zero while a managed custom screen is active so the original background video can remain visible. Native destination screens temporarily re-enable Scaleform; returning reopens the corresponding custom root or host screen. Escape at the main menu opens quit confirmation. Escape in a hub returns to the root; leaving an online host lobby also uses native party cleanup. `cslMenuRoute main` is a recovery route, while `leave` performs online lobby cleanup first.

Custom frontend pages close when the client begins connecting/loading and stay closed during gameplay, even if `sf_current_menu` still reports the previous lobby. The native return-to-main-menu transition enables the custom frontend again. Loading screens, HUDs and native pause menus are not replaced by this lifecycle guard.

Native decorations do not take button focus. PNG materials use private copies of the engine's 2D pipeline. `reloadMenus` rescans definitions only after they are closed. The parser, crop/fade timing, menu layout, Escape behavior, LIVE sign-in, host/guest permissions, return transitions, and display scaling require verification with the rebuilt client. Never treat a successful screen transition alone as proof of a valid party or match.

The local server browser is `csl_serverbrowser`, backed by QoS's native feeder `2`. Its five columns are server, map, players, mode and ping; refresh and join retain native discovery/connection behavior. Enter, double-click and Join Server request the selected server. Back and Escape return through `cslMenuRoute browserback` to the custom LOCAL PLAY hub, after the native browser screen has been left. Expanded disk definitions support `columns`, `doubleClick` and `selectborder` without macro preprocessing. The browser's native listbox layout and runtime navigation require a rebuilt DLL, not just updated menu files.

Loadouts opened from the System Link host (including its settings tab) go directly to QoS's native `mpcustomloadout` class editor using local sign-in, rather than the LIVE-gated `mploadout` shop wrapper. Online and private-online lobbies retain the original LIVE route. This does not introduce separate loadout storage or unlock equipment; native profile ownership still applies. Returning to the frontend restores the originating custom lobby. This routing change requires a rebuilt DLL and in-game validation.

## Launch Commands

Commands passed to `JB_Launcher_s.exe` with either a `+` or `-` prefix are forwarded to the multiplayer engine command buffer. Launcher-only switches such as `-multiplayer` and `-offline` are consumed by Consolation instead of being submitted as game commands.

```text
JB_Launcher_s.exe -multiplayer -offline -name mac-dev1 -seta com_maxfps 125 +devmap mp_barge
```
