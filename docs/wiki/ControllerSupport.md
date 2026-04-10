# Controller Support

This page describes current controller support and how it behaves in-game.

## Current Status (WIP)

- **XInput** is implemented and active, but still basic.
- **SDL2** backend now exists as a statically linked backend and is still very basic / WIP.
- **DirectInput** is not supported.
- **DualShock/DualSense** are planned as native SDL devices first, not via a full custom HID path yet.
- **Controller glyph/icon support** is planned later for Xbox and PlayStation button images in UI text.

## XInput Behavior

The XInput layer currently handles:
- Buttons and triggers via `CL_KeyEvent`
- Menu navigation (dpad + left stick)
- Analog movement into `usercmd`
- Analog look into native view input
- APAD direction keys from left stick

If a gamepad key has no binding in the engine, fallback commands are used.

## SDL Backend (Experimental)

`sdl_input` is no longer just a placeholder.

- It now builds against **SDL2 as a static library** from the vendored source tree
- You do **not** need to ship a separate `SDL2.dll` for this backend
- The current SDL path is intended for **PlayStation-style controllers first**
- This backend is still **very basic** and should be treated as work-in-progress

## Build Note

By default, `d3d9.dll` builds straight into the game install folder:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

Because that path is under `Program Files`, compiling or copying the output there usually requires Visual Studio to run with administrator rights.

## How Do I Obtain, Install, and Patch?

### Obtain

Obtain the game from **MyAbandonware**.

Only the **English/French** release is supported right now.

Unsupported setups include:

- repacks
- other regional releases
- other game versions

### Install

Install the game normally.

The recommended setup is the default install path:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

That avoids patch path issues and matches the included launcher shortcut by default.

### Patch

Install the official **1.1 patch** after installing the game.

Do **not** use unpatched `1.0`. It is currently unsupported.

When using either a nightly build or a release build:

- extract/copy the build into the game root
- overwrite everything when prompted
- launch with the `-multiplayer` argument

Example target:

- `"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer`

Nightly builds should ship with a `Launch Consolation.lnk` shortcut for this.

If your game is **not** installed in the default directory, edit the shortcut target so it points at your real install path.

Patch `1.1` may not install correctly if the game is not installed in the default `C:\` location.

Advanced users can still make a non-default install work by manually copying the patch `1.1` files into the game root and then editing `Launch Consolation.lnk` so it points at the correct `JB_Launcher_s.exe` path.

## Console Key

On a US keyboard layout, the console key is often the backtick key:

- `` ` ``

## Known Limitations

- No rumble / vibration yet
- No native DualShock / DualSense extras yet (touchpad, gyro, lightbar)
- No button glyph/icon material replacement yet
- Only the first controller is used
- SDL support is still early and currently focused on basic PlayStation-style controller detection/polling

## Planned Work

- Finish stabilizing the XInput implementation
- Finish stabilizing the SDL polling path
- Allow choosing backend (`XInput` vs `SDL`)
- Add proper Xbox / PlayStation button glyphs and localized icon text support
- Add fuller native PlayStation controller support through SDL, with HID-only work only if SDL proves too limited
