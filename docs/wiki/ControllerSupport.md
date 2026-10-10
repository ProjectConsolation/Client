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

Install the bundled `required_files/consolation/zone/common_consolation.ff` into the game's `consolation/zone/` directory, preserving the old file, then restart. It contains native 2D materials with inline images for all 32 Xbox/PS3 glyphs, the nine existing stance assets, and 15 original Xbox controller pictures/diagrams. Nightly packaging already includes this required zone. No loose controller PNGs are needed at runtime. Missing materials fall back to button names. Source glyphs live in `consolation/controller_assets/glyphs/`; original Xbox DDS sources live in `consolation/controller_assets/xbox/`, preserving their original names and compressed base-level pixels.

Controls > CONTROLLER > BUTTON LAYOUT / STICK LAYOUT now opens adapted original QoS Xbox Scaleform screens. Install the complete bundled `consolation/scaleform/` folder, including `CmOptCntlrBtns.gfx` and `CmOptCntlrStks.gfx`, not just the Controls movie. These reuse the original Xbox callouts, stick animations and full controller picture; the picture is embedded for PC rather than relying on the Xbox engine's drawing path. The full diagram remains Xbox artwork even when PS3 prompt glyphs are selected. Hovering previews a preset without changing bindings; SELECT applies the project's supported preset, and BACK keeps the current configuration. Custom button bindings are queried through the native key-binding callback on entry. No Xbox configuration files, rumble options or Xbox LIVE startup dependencies are imported. Reproduction uses `tools/patch_controller_diagrams.py` with FFDec 26.3 and the original Xbox movies; this does not build or install the client.

These sprites come from this game's console font atlases; only the presentation/mapping separation is informed by [IW4x](https://github.com/iw4x/iw4x-client/blob/main/src/Controller/Mapping/Glyph.cpp). QoS uses its own verified native inline-icon ABI, not IW4 material names or pointers in localized strings. Keyboard keys, mouse buttons, wheel events and foreground mouse movement immediately select PC binding text and stop controller view/aim assist, even with an icon-family override. To return to controller input, release PC keys/buttons, let all sticks/triggers return to their configured neutral range, and wait at least 750 ms after the last PC activity before a fresh controller gesture. A continuously moving stick, a held trigger, generated repeat, early gesture held through the timeout, or reconnect cannot bypass this gate. Aim integrators are cleared on PC takeover; native mouse look and melee processing still run. This prevents ordinary mixed-input abuse, not tampering with the client executable.

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
