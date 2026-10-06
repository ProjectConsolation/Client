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

Material pool capacity is separate from renderer capacity. The client also relocates
QoS's 1,626-entry sorted-material table so enumerating extended pools cannot overwrite
nearby renderer state. The native draw-surface format still has an 11-bit material
index: **2,048 simultaneously sorted materials** remains the rendering limit. The
4,096-entry pool is not a claim that all 4,096 can render simultaneously.

T4M also increases a separate `Loaded Sound` pool. QoS does not have that asset type: its index
10 is `Sound Curve`, while its existing `Sound` pool is 10,000 entries. Applying T4M's loaded-sound
count to either QoS type would therefore be incorrect.

These asset-pool changes allocate approximately 680 KiB of additional backing storage. They do
not alter the engine hunk or zone-memory limit. T4M's hard-coded `g_mem` addresses and byte values
belong to World at War and are not applied to QoS without a separate QoS-specific verification.

### [[Commands]]

Advanced helper commands such as `addbot`, `listassetpool`, `dvarDump`, and `commandDump`.

### Disk Image Overrides

Converted Xbox map probes temporarily use a six-face black cubemap when a world reflection probe cannot be decoded. QoS binds these images unconditionally; null probe references caused the renderer crash at `10385930`. Conversion reports list `reflection_probe_fallbacks`. This keeps the binding valid but **does not restore the original Xbox reflections**; their ARGB8 cubemap mip layout still needs conversion.

Place an IWI, DDS or PNG file in `consolation/images/<image-asset-name>.<extension>`
under the game installation. Use the **image** name, not its material name;
for example, `brick_wall_col` is overridden by
`consolation/images/brick_wall_col.iwi`. Image names containing subdirectories
retain them beneath `images`. This follows the loose-image naming convention
used by [IW4x](https://github.com/iw4x/iw4x-client/blob/main/src/Components/Modules/Materials.cpp),
but uses QoS's own texture-upload and ownership path rather than MW2 hooks.

Supported formats, up to 4096 pixels per dimension and 64 MiB per file:

- **IWI:** PC version 6, DXT1/DXT3/DXT5, 2D or square cubemap. Complete mip
  chain, or a single level with the no-mipmaps flag.
- **DDS:** legacy DXT1/DXT3/DXT5 and 32-bit RGBA/BGRA; DX10 BC1/BC2/BC3,
  RGBA8/BGRA8/BGRX8 UNORM. Complete power-of-two mip chain or a single level;
  cubemaps require all six faces. Padded rows, arrays, volumes, premultiplied
  alpha, BC4/BC5/BC6/BC7 and explicit sRGB formats are rejected.
- **PNG:** decoded by Windows Imaging Component to straight-alpha BGRA8,
  2D only. One mip level is uploaded; use DDS/IWI for authored mipmaps.

Priority is **PNG > DDS > IWI** when multiple files have the same basename.
An invalid highest-priority file retains the zone image, rather than silently
falling through to another file. Overrides load at full resolution; the original
material sampler, semantic and asset identity remain unchanged. Built-in,
procedural and render-target images are excluded. No normal-map conversion is
performed: a normal replacement must already use the PC shader's channel encoding.

Windows-illegal asset characters use percent-encoded filenames, including `%`
itself. For example, `*lightmap0_primary` uses `%2Alightmap0_primary.dds`.
Traversal paths and Windows device names are rejected.

Overrides are read when the fastfile image is uploaded, **not live when saved**.
Reload the zone that owns the image, or restart the game for shared images.
`[images] Loaded override` confirms replacement; invalid files produce an
`[images] Ignoring override` warning and retain the zone image. Removing the
file restores the original on its next load. This feature still requires
in-game validation with a rebuilt client.

#### Dumping Canals Images

`tools/xenon-converter/dump_pc_images.py` exports validated inline DXT images
from a **converted PC** fastfile into separate `iwi`, `dds` and optional `png`
directories. It refuses to overwrite an existing dump. The manifest records
original asset names, override basenames, pixel hashes and external images with
no embedded pixels. It does not fabricate missing/shared assets.

```powershell
python tools/xenon-converter/dump_pc_images.py tools/mp_canals.ff tools/image-dumps/my-canals-dump --png
```

PNG previews require Pillow; DDS/IWI export has no extra Python dependency.
Copy only the replacements you want into the game's `consolation/images`, using
the directory layout inside one of the format folders. The dump is not installed
automatically. Cubemap PNGs are explicitly labelled positive-X face previews;
use the whole DDS/IWI cube for an override. PNGs show the stored channels, including
encoded normals and lightmaps, not a reconstructed final lit material.

The current Canals dump contains 429 embedded images (including four lightmaps)
and identifies one shared `,$identitynormalmap` reference without embedded pixels.
All 429 exported DDS and IWI files were accepted by the override parser tests.

Format references: [Microsoft DDS layouts](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide),
[Windows PNG decoding](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-creating-decoder)
and local KisakCOD `r_image.h`/`r_image_load_common.cpp`. QoS PC upload and BGRA
support were verified at `0x103ADD20`, `0x10381D20` and `0x10381B60`.

#### Dumping Original Xbox Images

Use `dump_xenon_images.py` for the original big-endian Xbox fastfile, rather
than the converted PC zone. It uses the converter's Xenos untile/endian path,
without PC image donors or material substitutions:

```powershell
python tools/xenon-converter/dump_xenon_images.py path/to/mp_canals_xenon.ff tools/image-dumps/my-xbox-dump --png --pc-overrides --normal-slopes
```

`raw` preserves original tiled pixel blobs, with source offsets, hashes, headers
and resource metadata in `manifest.json`. `dds` and `png` contain untiled source
textures: DXN normals remain BC5/ATI2 in DDS. They are **not automatically
PC-compatible overrides**, particularly normal maps. `pc-overrides` separately
contains supported DDS/IWI copies, transcoding DXN to DXT5; `--normal-slopes`
selects the experimental QoS slope encoding. Ordinary source PNG normals are
not converted to that encoding. No replacement files are installed automatically.

Mip tails that the converter only approximates are excluded from the original
export; those images export a decoded base level instead, with the limitation
recorded in the manifest. Shared references with no pixels and unsupported
formats/cubemap layouts are listed rather than fabricated. The original Canals
zone currently yields 441 records: 429 decoded textures, six shared references,
and six undecoded embedded textures retained as raw blobs. This is an image
extraction tool, not a complete Xbox asset loader.

#### IWD Image Overrides

Place a classic ZIP archive named, for example, `csl_canals.iwd` in
`ROOT/consolation/main/` (any `.iwd` filename, including `XX_XX.iwd`).
Inside the archive use `images/<asset-name>.iwi`, `.dds`
or `.png`; **do not** include a leading `consolation/` directory. Filename
escaping and supported formats are the same as loose overrides above.
Loose `consolation/images` overrides win over archived images. Within each
source tier, PNG wins over DDS, then IWI. An invalid selected replacement
retains the zone image. Archive search ordering and pure-server restrictions
remain native engine behavior; this is not a purity bypass.

The client adds `consolation/main` archives to QoS's native search paths during
filesystem startup and restart. It does not change `fs_game` or redirect config
writes. QoS owns ZIP indexing, file handles, decompression and shutdown cleanup.
Restart the game after adding or replacing an IWD, and reload the owning zone
after changing a loose image. There is no live archive refresh. Native filesystem
consumers can also see other files in mounted IWDs; full mod compatibility has
not been established. This native integration needs a rebuilt-client in-game test.
The previous `ROOT/consolation/*.iwd` location is no longer mounted by this
feature; move those archives into `consolation/main`. Loose overrides stay in
`consolation/images`, not `consolation/main/images`.

Package just one format directory from a dump:

```powershell
python tools/xenon-converter/package_image_iwd.py tools/dumps/mp_canals/pc-overrides/iwi tools/dumps/mp_canals/csl_canals.iwd
```

The packager refuses overwrites, checks safe paths and size limits, uses classic
ZIP/Deflate and verifies every entry's CRC. The requested Xbox Canals dump is
in `tools/dumps/mp_canals`, including 429 **converted PC IWI v6** images. Xbox
pixels are embedded in the fastfile, not stored as original IWI files. Original
DXN normal DDS files remain BC5; use the separate PC overrides for this client.
The normal-map slope conversion is still experimental, not a guarantee of
correct world rendering. Keep `manifest.json` for the six missing/shared and
six undecoded records rather than treating the archive as a complete zone.

Native bindings were checked against local KisakCOD `com_files.cpp` and QoS
PC 1.1: `FS_Startup` at `0x10272D80`, archive mounting at `0x10271F30`, current-thread
open at `0x10271D60`, read at `0x10270840`, and EAX-handle close at `0x10270920`.

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

### Main-menu logo overlay

`consolation/menu/ui_mp/main_logo.menu` draws `consolation/images/logo_csl.png`
on the Scaleform `mpmainmenu` screen. Install the menu beside the existing disk
menus and rebuild/install the updated client DLL. Do **not** use `openmenu` for
this overlay: it is drawn automatically without entering the native menu stack,
changing the key catcher, taking focus, or processing mouse/keyboard events.

The `overlayFor "main"` and `scaleformMenu "mpmainmenu"` fields are Consolation
extensions, not stock COD4 grammar. Only decorative image items are permitted.
`rect -335 72 312 104 3 1` uses 480-high units and right/top alignment: approximately
702x234 pixels at 1920x1080, 52 pixels from the right edge. Size preserves the
PNG's 3:1 aspect ratio, scales with screen height, and stays right-anchored on
ultrawide. Safe-area dvars do not move it. `forecolor` supplies tint/opacity.

Use `reloadMenus` after changing the rectangle (close any interactive custom menu
first). PNG textures are cached for the process lifetime; replacing PNG pixels
requires restarting the game. Bad/missing PNG files log once instead of failing
the game. The overlay owns a managed D3D9 texture and a private copy of the stock
white 2D material; stock assets and DB upload globals are not modified. These
private textures are not included in the engine's zone image-memory accounting.
Main-screen visibility, alpha blending and Scaleform click-through still require
an in-game test; the client is not built automatically.

- [[Console]]

