# Patched Dvars

Project: Consolation unlocks, restores, adjusts, or adds several dvars that are useful for gameplay tweaking, testing, and quality-of-life changes.

## Patched Dvars

### Anti-aliasing and texture filtering

On the verified QoS PC 1.1 build, `r_aaSamples` accepts `1` through `16`.
`1` disables MSAA; higher values request that sample count. The default stays
`1`, and the dvar remains saved and latched. Apply with `vid_restart`, for example:

```text
seta r_aaSamples 8
vid_restart
gfxinfo
```

Requests of `1` through `4` retain the stock selector. Higher requests check
both A8R8G8B8 and QoS's selected depth format, descending to a supported count
or disabling MSAA if none works. Extended requests use quality index zero for
compatibility with auxiliary depth surfaces. The saved request is not replaced
by the fallback; `gfxinfo` distinguishes requested and actual settings.
Capability support is not a guarantee of successful allocation or performance;
test map loading, shadows, water, killcams, resizing and `vid_restart`. Revert
to `r_aaSamples 1` if higher modes fail. Patch-byte mismatches skip the extension.

Anisotropic filtering uses QoS's existing `r_texFilterAnisoMin` and
`r_texFilterAnisoMax` dvars (`1` to `16`, defaults `1` and `16`). For stronger
filtering of eligible mipped material samplers:

```text
seta r_texFilterAnisoMax 16
seta r_texFilterAnisoMin 16
```

Native filtering refreshes these settings at runtime and clamps them to GPU
support. Point-filtered/non-mipped samplers retain native policy; no global
sampler override is applied. AF cannot repair incorrect mipmaps or materials.
These changes retain D3D9, do not add D3D9Ex, and leave existing defaults intact.

First-person traversal toggles now accept native camera mode `1`, including
when snapshot parsing selects it for the native body camera. `pm_climbFirstPerson 1` requests first-person
ladder/ledge/pipe cameras, and `pm_mantleFirstPerson 1` requests first-person
mantling. `0` preserves native traversal cameras; explicit `cg_thirdPerson 2`
takes precedence over both. This changes camera selection, not traversal
collision or animations. Verify entry, movement and exit on each traversal type.

### Noclip Movement

While `noclip` is enabled, hold your existing jump binding to ascend and crouch
binding to descend. Holding both cancels vertical input. No new binds or dvars
are required. Movement follows the camera's up axis, matching the COD4 noclip
path; native speed scaling, friction and acceleration remain in use. Normal
walking, jumping, crouching and UFO movement are unchanged.

`pm_noclipScale` multiplies noclip movement speed in every direction, including
jump/crouch movement. It is saved, defaults to `1`, and accepts `0` to `20`.
For example, `pm_noclipScale 0.5` halves speed and `pm_noclipScale 2` doubles it.
`0` removes movement acceleration; existing momentum still slows through native
friction. Walking and UFO speed are not affected.

The QoS PC 1.1 patch restores vertical input at two noclip-only calculation
sites, guarded by expected instruction bytes. Verify ascent, descent, both
buttons together, diagonal movement and normal movement after leaving noclip.

### Dvar Reference

`bg_improvedMeleeLunge` is a server-replicated boolean, default `0` (stock QoS).
`bg_improvedMeleeLunge 1` replaces the native distance-times-scale launch speed
with COD4's distance-based `sqrt(2 * 1200 * distance)` launch. The shared
movement path applies it to server simulation and client prediction. It does
not widen hit range or targeting angles, change damage, or port COD4's entire
friction-timed charge state machine. Native QoS collision and subsequent
movement remain active. Verify player and bot melee on a listen server and
with remote clients before relying on the experimental option.

`scr_player_respawndelay` controls the player respawn wait in seconds. The
client adapts the original QoS `_tweakables.gsc` getter to read this setting
live instead of its server-start cache; changes also shorten an already-running
player-delay wait. Negative values are treated as zero.
Restart the client after installing the DLL so scripts are compiled with the
adaptation. Custom disk `_tweakables.gsc` overrides retain their own behavior.
`scr_player_forcerespawn 1` bypasses the native use-button prompt, including an
already-running prompt. `0` retains manual spawning. For immediate automatic
respawn use `set scr_player_respawndelay 0` and
`set scr_player_forcerespawn 1`. With killcam inactive, zero delay also skips the
death-camera hold after one server-frame yield needed to finish the death callback.
Team-wave waits, active killcam, limited-life modes and native mode-specific
respawn overrides remain separate policies. Custom disk `_globallogic.gsc`
overrides retain their own behavior. These controls adapt the built-in scripts;
users do not need to install their own scripts.

`sv_allowKillcams` is a server-owned boolean, default `1`. It enables the
existing QoS archived-snapshot killcam through its built-in `_killcam.gsc`;
it does not introduce a separate replay recorder. Console changes apply during
the match, and `sv_allowKillcams 0` also ends an active killcam through native
cleanup. Native eligibility rules, archive availability, skip input and
round-end limits remain in force. Custom disk `_killcam.gsc` overrides keep
their own policy. Restart with the updated DLL before testing script changes.
For immediate respawn without watching a killcam, additionally use
`set sv_allowKillcams 0`. Replay and its HUD require runtime verification;
the archive writer and replay consumer have been verified in the QoS PC binary.

| Name | Description | Default / Range |
| :--- | :--- | :--- |
| `r_fullscreen` | Made saved and writable so fullscreen behavior can be controlled more reliably and does not get forced back as aggressively by the stock game. | N/A |
| `com_maxfps` | Made saved and writable. Controls the frame rate cap. | N/A |
| `vid_xpos` | Made saved and writable. Controls the window position in windowed mode. | N/A |
| `vid_ypos` | Made saved and writable. Controls the window position in windowed mode. | N/A |
| `developer` | Registered with a `0` to `2` range. Enables the game's development environment behavior and is mainly useful for debugging or internal-style testing. | `0` to `2` |
| `g_speed` | Saved integer dvar. Controls player movement speed. | Default: `210` |
| `pm_noclipScale` | Saved speed multiplier applied only to noclip, including jump/crouch vertical movement. | Default: `1`, Range: `0` to `20` |
| `jump_height` | Saved float dvar. Controls the maximum jump height used by the player movement code. | Default: `41` |
| `cg_fov` | Saved float dvar. Controls the field of view angle in degrees. | Default: `65`, Range: `0` to `160` |
| `cg_fovScale` | Saved float dvar. Applies a multiplier to the base field of view. | Default: `1`, Range: `0` to `2` |
| `r_lodScale` | Saved float dvar. Adjusts level-of-detail distance. Higher values can keep more detail visible at range. | Default: `0`, Range: `0` to `3` |
| `input_viewSensitivity` | Saved float dvar. Controls mouse sensitivity. | Default: `1.0`, Range: `0.01` to `30.0` |
| `m_rawInput` | Saved boolean dvar. Enables raw mouse input handling. See also: [[Patched Raw Input]] | Boolean |
| `r_borderless` | Saved boolean dvar added by Project: Consolation. When used with windowed mode, this removes the normal window border. | Boolean |
| `ui_smallFont` | Saved float dvar. Adjusts the small UI font scale. | Range: `0` to `1` |
| `ui_bigFont` | Saved float dvar. Adjusts the large UI font scale. | Range: `0` to `1` |
| `ui_extraBigFont` | Saved float dvar. Adjusts the extra-large UI font scale. | Range: `0` to `1` |
| `cg_overheadNamesSize` | Saved float dvar. Adjusts the size of overhead player names. | Default: `0.5`, Range: `0` to `1` |
| `cg_overheadNamesFont` | Saved, cheat-protected font selector: `0` native, `1` normal, `2` big, `3` small, `4` bold. Missing fonts fall back to native. Retains the native subtle drop shadow. | Default: `2`, Range: `0` to `4` |
| `cg_drawWatermark` | Saved boolean dvar added by Project: Consolation. Draws the unshadowed Project: Consolation watermark at the top-right edge. | Default: `1` |
| `cg_drawVersion` | Saved boolean dvar added by Project: Consolation. Draws the blue build-version string on the bottom status line. | Default: `1` |
| `cg_drawVersionX` | Saved horizontal position from the left edge for the version string. | Default: `50`, Range: `-1024` to `1024` |
| `cg_drawVersionY` | Saved inset from the bottom edge for the version string. | Default: `17`, Range: `-1024` to `1024` |
| `cg_drawOrigin` | Draws live player origin and velocity below the active `cg_drawFPS` block. | Boolean |
| `cg_drawMaterial` | `0` disables material diagnostics; `1` retains the stock material, surface, and contents labels; `2` adds the resolved render material's techset and texture/image bindings below them, using the same native diagnostic font, size and screen placement. Preserves the native trace and cheat protection. Replaces the separate `cg_drawTechset` toggle. | Integer (`0`-`2`) |
| `cg_drawFPS` | Saved debug overlay mode. `1` draws FPS only; values `2` and higher draw the standard three-line FPS, frame-time, and triangle block. | `0` to `7` |
| `cg_drawMemInfo` | Saved live memory overlay, centered at the right edge. Mode `1` shows working, private, peak, then a blank line and free process address space in MB. Mode `2` reproduces the native `meminfo` hunk, physical-memory, and high/low zone breakdown; mode `3` shows those same quantities in bytes. Memory pressure is colored yellow/red. | Default: `0`, Range: `0` to `3` |
| `cg_debugInfoCornerOffset` | Default value corrected to `0 0`. Affects the corner offset used by some debug-style HUD info such as `cg_drawFPS`. | Default: `0 0` |
| `safeArea_horizontal` | Saved horizontal safe-area fraction used by HUD screen placement. | Default: `0.85`, Range: `0` to `1` |
| `safeArea_vertical` | Saved vertical safe-area fraction used by HUD screen placement. | Default: `0.85`, Range: `0` to `1` |
| `monkeytoy` | Registered as writable. Useful for modding and development-oriented workflows where the stock restrictions are not wanted. | N/A |
| `g_debugVelocity` | Custom debug boolean dvar. Prints velocity-related debug information to the console. | Boolean |
| `g_debugLocalization` | Custom debug boolean dvar. Prints information about unlocalized strings to the console. | Boolean |
| `bot_maxHealth` | Cheat-protected integer dvar. Initialized from `scr_player_maxhealth` per server; later edits apply on each bot’s next respawn. | Fallback: `100`, Range: `1` to `1000` |

## Notes

- Dvar lookup uses QoS' locked native lookup. Client registration uses the
  native find-or-re-register path instead of allocating a second entry for
  an existing name. The periodic UI override callback updates existing
  variables only; the native 3,000-dvar limit is not increased.
- Native boolean writes to the extended `cg_thirdPerson` integer are adapted
  to `0`/`1`. Console mode `2` remains available, and native protection and
  domain checks still apply. These changes require runtime regression testing
  in both normal GFWL and optional offline mode after rebuilding.

- Some of these dvars are stock dvars that have been made writable or saved.
- Some are custom dvars added by Project: Consolation.
- A few are mainly intended for testing, debugging, or modding.
- Not every patched dvar is guaranteed to behave perfectly in every mode.
- If a value seems unstable, gets reset by the game, or causes odd behavior, return it to a safer default and test again before assuming the feature itself is broken.
