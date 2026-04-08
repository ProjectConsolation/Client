# Controller Support

This page describes current controller support and how it behaves in-game.

## Current Status (WIP)

- **XInput** is implemented and active, but very basic.
- **SDL** backend is not active yet (no live input).
- **DirectInput** is not supported.
- **DualShock/DualSense** work only if emulated as XInput (e.g. DS4Windows / Steam Input).

## XInput Behavior

The XInput layer handles:
- Buttons and triggers via `CL_KeyEvent`
- Menu navigation (dpad + left stick)
- Analog movement into `usercmd`
- Analog look into native view input
- APAD direction keys from left stick

If a gamepad key has no binding in the engine, fallback commands are used.

## SDL Backend (Planned)

`sdl_input` is a stub that defines normalized state conversion and button mapping only.  
No device polling or SDL linkage is implemented yet.

## Known Limitations

- No rumble/vibration yet
- No native DualShock/DualSense extras (touchpad, gyro, lightbar)
- Only the first controller is used

## Planned Work

- Add SDL device polling
- Allow choosing backend (XInput vs SDL)
- Native PlayStation controller support (DualShock/DualSense) via SDL or HID
