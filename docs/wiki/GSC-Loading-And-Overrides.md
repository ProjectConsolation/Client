# GSC Loading And Overrides

Project: Consolation supports custom GSC loading and overriding stock scripts.

## How It Works

Create a `consolation` folder inside your game directory.

Inside that folder, follow the same folder structure as the stock game scripts you want to override.

## Example

If you want to override the stock Conflict gametype script, create:

`consolation/maps/mp/gametypes/dm.gsc`

This lets you replace or extend the stock `dm.gsc` behavior through the Project: Consolation script loading path.

## Practical Use

One use for this is making a custom FFA-style Conflict variant by replacing `dm.gsc` with your own logic.

Example showcase:

[![Custom Gun Game example by replacing `dm.gsc`](https://img.youtube.com/vi/0Zu-5G9qdcg/hqdefault.jpg)](https://www.youtube.com/watch?v=0Zu-5G9qdcg)

## Live Reload

While a level is running, Project: Consolation watches every disk-backed `.gsc` that was compiled for that level. Saving one of those files compiles a new bytecode generation immediately and prints:

`reloading script file <path>`

Future calls to functions present in both generations are redirected to the newest bytecode without `map_restart` or restarting the level. Existing threads keep their old instruction stream until they enter a reloaded function; this preserves active VM stack frames instead of attempting to relocate a running thread into changed bytecode.

The reloader does not run `main` or `init` again, because doing so can duplicate entities, callbacks, and long-running threads. Newly added functions are available to other functions in the new generation, while a removed function remains callable from older bytecode until the level ends. Reload generations consume space in the level's fixed script bytecode arena and are released through the normal script shutdown path.
