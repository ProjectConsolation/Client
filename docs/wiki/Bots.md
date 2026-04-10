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

## Notes

- Bots are mainly intended for testing, casual play, and local experimentation right now.
- They are not a replacement for polished single-player-style AI.
- Some maps, modes, or edge cases may still expose rough behavior.
