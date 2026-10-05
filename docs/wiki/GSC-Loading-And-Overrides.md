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

Live GSC reload is currently disabled. The experimental implementation bypassed native script finalization and intercepted the VM dispatcher, which could corrupt script variable state during map startup. Disk-backed script loading and overrides remain supported; restart the map after changing a script.

### October 5 investigation

The QoS PC finalizer at `0x1022E300` clears the script-loading flag at
`0x118D5250` and releases and zeros four compiler object roots at
`0x118B5238` through `0x118B5244`. This matches the lifecycle in KisakCOD's
`src/script/scr_main.cpp`: `Scr_EndLoadScripts` is an ownership transition,
not just a flag change. Some existing IDA callee names in this routine are
misleading; they are not evidence that these are animation objects.

The dormant reload code intentionally skips that cleanup and later sets the
loading flag again to compile a generation alias. It also redirects VM code
positions. Re-enabling the watcher alone would re-enable these unsafe lifetime
assumptions; the current investigation does not establish a safe live compiler
transaction or prove an exact cause for every previous crash.

A replacement needs a verified compiler-state transaction at a VM-safe point,
recoverable compile failures, atomic function-map publication, and a bounded
code-memory policy. Old bytecode must stay alive while any suspended or running
thread references it. File watching also needs save debouncing and retries:
the dormant watcher marks a timestamp consumed before reading or compiling,
which can miss a failed or partial save. Native finalization and VM dispatch
remain enabled while these requirements are unresolved. No live-reload feature
has been re-enabled by this investigation.
