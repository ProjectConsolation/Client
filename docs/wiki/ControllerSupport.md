# Controller Support

This page describes current controller support and how it behaves in-game.

## Current Status (WIP)

- **XInput** is implemented and active, but still basic.
- **SDL2** backend now exists as a statically linked backend and is still very basic / WIP.
- **DirectInput** is not supported.
- **DualShock/DualSense** are planned as native SDL devices first, not via a full custom HID path yet.
- **Controller binding glyphs** use the extracted Xbox/PS3 artwork in native in-game prompts. Rebuild and install the bundled images before testing.

## XInput Behavior

The XInput layer currently handles:
- Buttons and triggers via `CL_KeyEvent`
- Menu navigation (dpad + left stick)
- Analog movement into `usercmd`
- Analog look into native view input
- APAD direction keys from left stick
- Accelerated held-menu repeat for stick and dpad navigation

If a gamepad key has no binding in the engine, fallback commands are used.

## Credits

- Menu repeat acceleration tuning in the gamepad work includes an implementation credit to GitHub user `not-czar`.
- Controller movement and input behavior continue to be informed by IW3SP and IW4x controller research and code structure.

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

## Console Key

On a US keyboard layout, the console key is often the backtick key:

- `` ` ``

## Controller Icons

`ui_controllerIcons` is an archived string setting: `auto` (default), `xbox`, or `ps3`. Auto follows the controller being used: XInput selects Xbox artwork; other supported controller families select PS3 artwork. An explicit value overrides artwork only, not input mapping. Unknown values behave as auto.

```text
seta ui_controllerIcons auto
seta ui_controllerIcons xbox
seta ui_controllerIcons ps3
controller_status
```

The existing `controller_status` command reports the active input source, override and resolved icon family. Keyboard/mouse activity restores keyboard prompts; controller navigation does not falsely count as keyboard input. Prompts query live bindings, including manual remaps, rather than assuming the default button layout. Native use/reload, stance, melee and breath aliases remain intact, and saved binding names stay readable `BUTTON_*` names.

Install all 32 `controller_xbox_*.png` and `controller_ps3_*.png` images from `required_files/consolation/images/` into the game's `consolation/images/` directory, then restart. Nightly packaging includes these through the existing recursive required-files copy. Missing artwork falls back to button names. These sprites come from this game's console font atlases; only the presentation/mapping separation is informed by [IW4x](https://github.com/iw4x/iw4x-client/blob/main/src/Controller/Mapping/Glyph.cpp). QoS uses its own verified native inline-icon ABI, not IW4 material names or pointers in localized strings.

An XInput wrapper cannot identify the physical controller behind it; use the `ps3` override for a PlayStation pad exposed as XInput. This path covers native engine binding prompts, not every decorative icon or label embedded in a Scaleform movie. Runtime validation remains required: test reload/use, jump, melee, manual rebinding, keyboard switching, unplug/reconnect and a renderer restart with both families.

## Known Limitations

- No rumble / vibration yet
- No native DualShock / DualSense extras yet (touchpad, gyro, lightbar)
- Only the first controller is used
- SDL support is still early and currently focused on basic PlayStation-style controller detection/polling

## Planned Work

- Finish stabilizing the XInput implementation
- Finish stabilizing the SDL polling path
- Allow choosing backend (`XInput` vs `SDL`)
- Add fuller native PlayStation controller support through SDL, with HID-only work only if SDL proves too limited
