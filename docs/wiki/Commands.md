# Commands

Project: Consolation includes a small set of utility and debugging commands that are useful for advanced users, modders, and troubleshooting.

## Commands

| Name | Description | Example |
| :--- | :--- | :--- |
| `addbot [count]` | Spawns one or more multiplayer bots and assigns names from `consolation/bots.txt` when available. | `addbot` `addbot 4` |
| `dvarDump [filename]` | Prints all registered dvars to the console and can optionally write them to a text file under the `consolation` folder. | `dvarDump` `dvarDump dvars` |
| `commandDump [filename]` | Prints all registered command names to the console and can optionally write them to a text file under the `consolation` folder. | `commandDump` `commandDump commands` |
| `listassetpool <poolnumber> [filter]` | Lists assets from the selected asset pool and can optionally filter the output by text. | `listassetpool 0` `listassetpool 13 weapon` |
| `origin [0|1]` | Debug builds only. Toggles a twice-per-second origin and velocity status line in the internal game console; an explicit value enables or disables it. | `origin` `origin 1` `origin 0` |
| `cslMenuRoute <options\|host\|join\|online\|main>` | Native Consolation main-menu handoff to an existing QoS Scaleform screen. `main` returns to the root. Does not provide online service emulation. | `cslMenuRoute options` |
| `openmenu <name>` | Opens a parsed disk menu when available, otherwise uses native asset lookup. | `openmenu csl_main` |
| `reloadMenus` | Rescans disk definitions. Refuses to replace menus still in the native open stack. | `reloadMenus` |
| `bind <key> "<command>"` | Uses native key binding with the stock-action whitelist removed. Command case, quoting, key-up handling and config persistence remain native. | `bind F5 "noclip"` |

## Notes

- If you provide a filename to `dvarDump` or `commandDump`, `.txt` is added automatically if needed.
- `commandDump` is still rough and may produce mangled or incomplete results in some cases.
- `listassetpool` can also be run without arguments to print the available pool numbers and asset type names.

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

Development implementation; these menu changes require the matching client source and a rebuild. They have not yet been validated in game.

`consolation/menu/ui_mp/main.menu` defines the SM2-inspired frontend: six horizontal navigation tabs, three large play cards, a right-aligned `logo_csl.png` at 65% opacity, dark panels, red selection accents, and a help line. A native fullscreen black fill at 35% opacity dims the original background video; no black PNG is required. The supplied compiled MW2/SM2 menus inform navigation, card and popup styling, while scripts and layouts are adapted to QoS rather than importing MW2 expressions or ABI. The native focus/action/dialog pattern follows the [COD4 Mod Tools main menu](https://github.com/promod/CoD4-Mod-Tools/blob/master/raw/ui_mp/main.menu). Syntax references: [OpenAssetTools](https://openassettools.dev/asset/menu/structure.html) and [Quake III menu guide](https://icculus.org/~phaethon/q3tamenu/q3tamenu-1.html).

Slideshow backgrounds (`slideshow_mp.png` / `slideshow_sp.png`) select the corresponding `preview_mp_*.png` / `preview_sp_*.png` files. Previews preserve aspect ratio by cropping to cover their card, pan slowly in alternating directions, advance every eight seconds and crossfade for 1.5 seconds. They render at the native window-background stage, underneath labels and interaction layers. Menu scripts are normalized to a single-line token stream for QoS's native runner, preserving quoted command strings and removing comments.

The client opens `csl_main` when the stock native `main` host is active and Scaleform reports `mpmainmenu`. It also observes return from a Scaleform submenu and can reopen the root when that submenu left an empty native menu stack. The stock host remains underneath where available for compatibility with existing submenus. `sf_enable` is set to zero while the custom root is active. Handoff to a Scaleform destination enables it even if `sf_enable` was previously zero; otherwise the previous setting is restored. Options and the System Link create/join screens still use their original Scaleform implementations; the lobby is not replaced. If a handoff fails, `cslMenuRoute main` resets it. The script normalization, return handling and animated previews have standalone test coverage where applicable; the updated native hooks still require in-game validation after rebuilding.

Cards cycle through `consolation/images/preview_mp_*.png` and `preview_sp_*.png` every six seconds. `reloadMenus` clears the preview list once the custom menus are closed. The current campaign previews temporarily reuse DLC multiplayer artwork; they are not campaign screenshots. Missing previews leave the card panel visible, never a null material draw. Escape opens quit confirmation at the root; the footer uses white key names and gray action labels.

Quit and switching to single player require confirmation; Escape cancels a dialog. The native decoration flag prevents the logo, background and help text from taking button focus. PNG materials are private copies using the engine's 2D pipeline, uploaded lazily once the renderer is available. Root braces are supported, but preprocessing and expression compilation are still not implemented.

Windows LIVE and Guide retain their original service requirements. In offline mode these entries do not create an online service or a replacement Guide. Mouse/keyboard navigation, submenu return, display scaling, and LIVE sign-in behavior require in-game verification after rebuilding the client.

## Launch Commands

Commands passed to `JB_Launcher_s.exe` with either a `+` or `-` prefix are forwarded to the multiplayer engine command buffer. Launcher-only switches such as `-multiplayer` and `-offline` are consumed by Consolation instead of being submitted as game commands.

```text
JB_Launcher_s.exe -multiplayer -offline -name mac-dev1 -seta com_maxfps 125 +devmap mp_barge
```
