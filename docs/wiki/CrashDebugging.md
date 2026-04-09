# Crash Debugging

This page documents how Consolation writes crash and hang dumps, where logs land, and which launch flags affect behavior.

## Build Output Note

By default, the generated `d3d9.dll` is written to:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

That means local builds usually need administrator rights, because the output path is inside `Program Files`.

## Crash Dumps

Consolation writes `.dmp` files into a `minidumps` folder under the game working directory.

Examples:
- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\minidumps\consolation-crash-<timestamp>.dmp`
- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\minidumps\consolation-hang-<timestamp>.dmp`

## Hang Watchdog

The watchdog monitors the main thread heartbeat. If it stops updating for 10 seconds, the watchdog:
1. Writes a hang dump (`consolation-hang-*.dmp`)
2. Terminates the process

To disable the watchdog:
- Launch with `-no_watchdog`

## Exception Handler

The crash handler installs an exception filter that writes a minidump on crash.

Flags:
- `-crashdump` installs the handler immediately
- `-no_crashdump` disables crash dumps entirely

## Boot and Engine Logs

Two logs are written to help with early startup issues:

- Boot log (component loader):  
  `%LOCALAPPDATA%\Consolation\logs\boot.log`

- Engine log (Com_Printf output):  
  `%LOCALAPPDATA%\Consolation\logs\engine.log`

If the game stalls without crashing, share the last ~50 lines of both logs.
