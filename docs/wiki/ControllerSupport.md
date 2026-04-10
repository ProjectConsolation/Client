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
