# Controller Support

This page describes current controller support and how it behaves in-game.

## Current Status (WIP)

- **XInput** is implemented and active, but still basic.
- **SDL2** backend now exists as an optional runtime-loaded path and is still very basic / WIP.
- **DirectInput** is not supported.
- **DualShock/DualSense** are planned as native SDL devices first, not via a full custom HID path yet.

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

- It now uses **runtime loading** for `SDL2.dll` / `SDL2-2.0.dll`
- If no SDL2 runtime is present, the backend stays inactive and the game falls back to the existing XInput-only behavior
- The current SDL path is intended for **PlayStation-style controllers first**
- This backend is still **very basic** and should be treated as work-in-progress

## Build Note

By default, `d3d9.dll` builds straight into the game install folder:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

Because that path is under `Program Files`, compiling or copying the output there usually requires Visual Studio to run with administrator rights.

## Known Limitations

- No rumble / vibration yet
- No native DualShock / DualSense extras yet (touchpad, gyro, lightbar)
- Only the first controller is used
- SDL support currently depends on a compatible SDL2 runtime being present next to the game or otherwise loadable by Windows

## Planned Work

- Finish stabilizing the XInput implementation
- Finish stabilizing the SDL polling path
- Allow choosing backend (`XInput` vs `SDL`)
- Add fuller native PlayStation controller support through SDL, with HID-only work only if SDL proves too limited
