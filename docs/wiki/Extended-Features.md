# Extended Features

This section documents custom features added by Project: Consolation beyond the stock PC version of *007: Quantum of Solace*.

## Features

### [[Bots]]

Experimental multiplayer bot support with an `addbot` command, custom bot names, and better behavior than the stock baseline.

### [[Ultrawide Support]]

Experimental custom ultrawide resolution and aspect-ratio support for gameplay.

### [[Patched Dvars]]

Documents stock dvars that have been unlocked or adjusted, as well as custom dvars added by Project: Consolation.

### [[Patched Raw Input]]

Input-related improvements for raw mouse handling.

### [[Profile Config Tools]]

Utility commands for converting, deduping, and locating the active Project: Consolation config.

### [[Commands]]

Advanced helper commands such as `addbot`, `listassetpool`, `dvarDump`, and `commandDump`.

### Custom Branding

Project: Consolation embeds its multiplayer artwork in `d3d9.dll` and intercepts the User32
`LoadIconA` and `LoadImageA` calls made by `jb_mp_s.dll`. A build uses these replaceable source
assets:

| File | Purpose | Expected format |
| --- | --- | --- |
| `required_files/icon.ico` | Game and window icon | Windows icon containing a 256x256 image |
| `required_files/jb.bmp` | Startup splash | 768x480, 24-bit BMP |
| `required_files/jblogo.bmp` | External console banner | 610x60, 24-bit BMP |

Replace the source files before building to change the embedded artwork. Loose bitmap files are
not needed at runtime. The stock multiplayer DLL's exact `jb.bmp` and `jblogo.bmp` requests are
overridden; unrelated User32 image requests continue to the original functions.

### Other Extended Features

- [[Console]]

