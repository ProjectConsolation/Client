# Patched Dvars

Project: Consolation unlocks, restores, adjusts, or adds several dvars that are useful for gameplay tweaking, testing, and quality-of-life changes.

## Patched Dvars

| Name | Description | Default / Range |
| :--- | :--- | :--- |
| `r_fullscreen` | Made saved and writable so fullscreen behavior can be controlled more reliably and does not get forced back as aggressively by the stock game. | N/A |
| `com_maxfps` | Made saved and writable. Controls the frame rate cap. | N/A |
| `vid_xpos` | Made saved and writable. Controls the window position in windowed mode. | N/A |
| `vid_ypos` | Made saved and writable. Controls the window position in windowed mode. | N/A |
| `developer` | Registered with a `0` to `2` range. Enables the game's development environment behavior and is mainly useful for debugging or internal-style testing. | `0` to `2` |
| `g_speed` | Saved integer dvar. Controls player movement speed. | Default: `210` |
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
| `cg_drawVersion` | Saved boolean dvar added by Project: Consolation. Draws the build version string in the bottom-right corner. | Boolean |
| `cg_debugInfoCornerOffset` | Default value corrected to `0 0`. Affects the corner offset used by some debug-style HUD info such as `cg_drawFPS`. | Default: `0 0` |
| `monkeytoy` | Registered as writable. Useful for modding and development-oriented workflows where the stock restrictions are not wanted. | N/A |
| `g_debugVelocity` | Custom debug boolean dvar. Prints velocity-related debug information to the console. | Boolean |
| `g_debugLocalization` | Custom debug boolean dvar. Prints information about unlocalized strings to the console. | Boolean |
| `bot_maxHealth` | Custom integer dvar. Controls the health bots receive when they spawn. | Default: `100`, Range: `1` to `1000` |

## Notes

- Some of these dvars are stock dvars that have been made writable or saved.
- Some are custom dvars added by Project: Consolation.
- A few are mainly intended for testing, debugging, or modding.
- Not every patched dvar is guaranteed to behave perfectly in every mode.
- If a value seems unstable, gets reset by the game, or causes odd behavior, return it to a safer default and test again before assuming the feature itself is broken.