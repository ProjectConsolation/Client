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

### `bot_maxHealth`

Custom integer dvar with a default of `100` and a range of `1` to `1000`.

This controls how much health bots receive when they spawn.
Use `bot_maxHealth 100` for the default health. Runtime health checks only cap
values above the configured maximum; they do not refill damage or restore
zero/negative health. Changing the limit does not heal existing bots.

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

- Bots are mainly intended for testing, casual play, and local experimentation right now.
- They are not a replacement for polished single-player-style AI.
- Some maps, modes, or edge cases may still expose rough behavior.
