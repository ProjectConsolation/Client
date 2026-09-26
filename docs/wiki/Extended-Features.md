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

### Increased Asset Limits

Project: Consolation increases selected fastfile asset pools to targets used by
[T4M](https://github.com/iAmThatMichael/T4M), described there as similar to or greater than
*Black Ops* (T5) limits. The implementation is not a direct World at War patch: each asset-type
index, stock count, backing-pool address, initializer address, and element stride was verified
against the supported *Quantum of Solace* PC 1.1 multiplayer DLL.

| Asset type | Stock QoS limit | Consolation limit |
| --- | ---: | ---: |
| FX | 340 | 600 |
| Image | 2,800 | 4,096 |
| Material | 1,626 | 4,096 |
| Stringtable | 5 | 80 |
| Weapon | 256 | 320 |
| Xmodel | 640 | 1,500 |

T4M also increases a separate `Loaded Sound` pool. QoS does not have that asset type: its index
10 is `Sound Curve`, while its existing `Sound` pool is 10,000 entries. Applying T4M's loaded-sound
count to either QoS type would therefore be incorrect.

These asset-pool changes allocate approximately 680 KiB of additional backing storage. They do
not alter the engine hunk or zone-memory limit. T4M's hard-coded `g_mem` addresses and byte values
belong to World at War and are not applied to QoS without a separate QoS-specific verification.

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

