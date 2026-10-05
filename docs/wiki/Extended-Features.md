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

### Disk Image Overrides

Place a PC IWI version 6 file in `consolation/images/<image-asset-name>.iwi`
under the game installation. Use the **image** name, not its material name;
for example, `brick_wall_col` is overridden by
`consolation/images/brick_wall_col.iwi`. Image names containing subdirectories
retain them beneath `images`. This follows the loose-image naming convention
used by [IW4x](https://github.com/iw4x/iw4x-client/blob/main/src/Components/Modules/Materials.cpp),
but uses QoS's own texture-upload and ownership path rather than MW2 hooks.

The initial implementation supports DXT1, DXT3 and DXT5 2D images and square
cubemaps, up to 4096 pixels per dimension and 64 MiB per file. Include the complete
mip chain unless the IWI no-mipmaps flag is set. Overrides load at full resolution;
the original image's material sampler, semantic and asset identity remain unchanged.
PNG, DDS, newer IWI versions, DXN, volume/streaming and legacy-normal encodings
are not supported. Built-in, procedural and render-target images are excluded.

Overrides are read when the fastfile image is uploaded, **not live when saved**.
Reload the zone that owns the image, or restart the game for shared images.
`[images] Loaded override` confirms replacement; invalid files produce an
`[images] Ignoring override` warning and retain the zone image. Removing the
file restores the original on its next load. This feature still requires
in-game validation with a rebuilt client.

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

