# Controller Support

This page describes current controller support and how it behaves in-game.

## Current Status (WIP)

- **XInput** is implemented and active, but still basic.
- **SDL** backend exists as a scaffold only and is not active yet.
- **DirectInput** is not supported.
- **DualShock/DualSense** work only if emulated as XInput (for example DS4Windows / Steam Input).

## XInput Behavior

The XInput layer currently handles:
- Buttons and triggers via `CL_KeyEvent`
- Menu navigation (dpad + left stick)
- Analog movement into `usercmd`
- Analog look into native view input
- APAD direction keys from left stick

If a gamepad key has no binding in the engine, fallback commands are used.

## SDL Backend (Planned)

`sdl_input` is currently a stub only.  
It is present in the project layout so the backend split is in place, but there is no live SDL device polling or SDL linkage yet.

## Build Note

By default, `d3d9.dll` builds straight into the game install folder:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

Because that path is under `Program Files`, compiling or copying the output there usually requires Visual Studio to run with administrator rights.

## Known Limitations

- No rumble / vibration yet
- No native DualShock / DualSense extras yet (touchpad, gyro, lightbar)
- Only the first controller is used

## Planned Work

- Finish stabilizing the XInput implementation
- Add SDL device polling
- Allow choosing backend (`XInput` vs `SDL`)
- Add planned native PlayStation controller support through SDL or HID
