# Bots

Project: Consolation includes basic multiplayer bot support.

Bots are still experimental and still pretty dumb sometimes, but they are more capable than the stock placeholder-style behavior.

## Main Command

- `addbot [count]`: spawn one or more bots into the current server

Examples:

- `addbot`
- `addbot 6`

## What They Do Better

- pick targets and remember them for a short time
- avoid obviously targeting teammates
- use basic visibility checks before firing
- crouch, sprint, strafe, and melee in some situations
- try to recover when stuck
- look for simple cover offsets during combat

## Names

Bot names are loaded from:

`consolation/bots.txt`

If that file is missing or empty, Project: Consolation falls back to generated bot names.

## Related Dvar

### `bot_difficulty`

Live server-side integer setting, range `0`–`3`, default `0` (Easy).
No cheats or bot respawn are required. It does not change bot health.

| Value | Level | Turn speed | Reaction | Fire burst / cycle | Aim error |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 0 | Easy | 45°/s | 900 ms | 100 / 1400 ms | ±4° |
| 1 | Regular | 75°/s | 650 ms | 130 / 1000 ms | ±2.5° |
| 2 | Hardened | 100°/s | 450 ms | 160 / 800 ms | ±1.25° |
| 3 | Veteran | 120°/s | 350 ms | 180 / 650 ms | ±0.5° |

Use `bot_difficulty 0` for the gentlest setting. Easy and Regular do not
aim down sights; aim error varies over time rather than locking onto an exact
point. Selecting a different visible enemy restarts the reaction timer.
Melee also uses reaction/burst timing. These are custom combat policies,
not restored native QoS difficulty levels.

### `bot_maxHealth`

Cheat-protected integer dvar with a range of `1` to `1000`. Each new server
initializes it from `scr_player_maxhealth` after script initialization; an absent,
invalid or out-of-range setting falls back to `100`. Bot entity health, the
actual player-state health stat and both maximum-health fields use this limit,
even if a native loadout would otherwise supply a different maximum.
Bot commands preserve the selected loadout; stuck recovery no longer writes
a bogus class index that could trigger native loadout/health initialization.

After server startup, enable `sv_cheats 1` and set `bot_maxHealth 200` (for
example). The new value applies when each bot next respawns, not to its current
life. Kill existing bots once to test the changed spawn health. Runtime checks
retain each life’s chosen maximum without healing wounds or reviving dead bots.
Starting another server/map initializes the setting from game settings again.

## Notes

Target selection adapts threat/sight separation and close-target retention from
KisakBlack's Combat Training source, using QoS's own trace helper and client
layouts rather than BO1 bindings. Candidate visibility is reused for up to
150 ms, invalidated by significant observer/target movement, and limited to
2800 world units. The selected enemy is checked freshly before firing; both
pitch and yaw must be aligned. Bots favor keeping close visible enemies and
only investigate hidden enemies from remembered sightings or recent gunfire,
instead of automatically pursuing every unseen player.

This is not a complete Combat Training port: BO1 navigation, damage-threat
callbacks and the full perception/weapon subsystem are not implemented here.

Combat commands compensate for native spawn angle offsets and check actual
view angles before firing. Bots stop sprinting when engaging within firing
range. Difficulty-dependent fire bursts and releases allow semi-automatic
weapons to retrigger; the native `sv_botsPressAttackBtn` gate also disables
close-range melee. This timing policy adapts KisakBlack's fire-delay approach,
but does not import BO1 weapon definitions or ammo-management logic.

Health synchronization caps both entity health and QoS's actual player-state
health stat, and updates its persistent and replicated maximum-health fields.

- Bots are mainly intended for testing, casual play, and local experimentation right now.
- They are not a replacement for polished single-player-style AI.
- Some maps, modes, or edge cases may still expose rough behavior.
