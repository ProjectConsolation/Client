# Controller Support

This page describes current controller support and how it behaves in-game.

## Current Status (WIP)

- **XInput** is implemented and active.
- **SDL2** backend now exists as a statically linked backend and is still very basic / WIP.
- **DirectInput** is not supported.
- **DualShock/DualSense** have native HID input paths alongside the experimental SDL backend.
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

Install the bundled `required_files/consolation/zone/common_consolation.ff` into the game's `consolation/zone/` directory, preserving the old file, then restart. It contains 67 native 2D materials with inline images: all 32 Xbox/PS3 glyphs, the nine existing stance assets, 15 original Xbox pictures/diagrams and 11 original PS3 pictures/diagrams. Nightly packaging includes this required zone and checks its packaged hash alongside all 13 menu movies. No loose controller PNGs or DDS files are needed at runtime. Missing materials fall back to button names. Source glyphs live in `consolation/controller_assets/glyphs/`; original console DDS sources live in `consolation/controller_assets/xbox/` and `consolation/controller_assets/ps3/`, preserving their original names and compressed base-level pixels. `tools/extend_common_controller_art.py` adds missing console artwork to the supported existing common zone without re-encoding its stance or glyph textures; the full packaging tools also include both console families.

Controls > CONTROLLER > BUTTON LAYOUT / STICK LAYOUT opens adapted original QoS console Scaleform screens. Install the complete bundled `consolation/scaleform/` folder, including `CmOptCntlrBtns.gfx` and `CmOptCntlrStks.gfx`, not just the Controls movie. These retain the original callouts and stick animations; the full Xbox and PS3 controller pictures are embedded for PC rather than relying on the console engine's drawing path. Both pictures follow the same `ui_controllerIcons` override and resolved automatic family as prompt glyphs, updating while the screen is open. Keyboard/mouse takeover changes binding prompts but does not turn a PlayStation controller diagram into an Xbox diagram. The original PS3 picture source lives in `consolation/controller_assets/ps3/`. Hovering previews a preset without changing bindings; SELECT applies the project's supported preset, and BACK keeps the current configuration. Custom button bindings are queried through the native key-binding callback on entry. No console configuration files, rumble options or Xbox LIVE startup dependencies are imported. Reproduction uses `tools/patch_controller_diagrams.py` with FFDec 26.3, the original Xbox movies and both extracted controller pictures; `--upgrade` updates an existing frontend bundle while preserving its other menu work. This does not build or install the client.

These sprites come from this game's console font atlases; the presentation/mapping separation is informed by [IW4x](https://github.com/iw4x/iw4x-client/blob/main/src/Controller/Mapping/Glyph.cpp). QoS uses its own verified native inline-icon ABI, not IW4 addresses, material names or pointers in localized strings. Keyboard keys, mouse buttons, wheel events and foreground mouse movement select PC binding text and stop controller view/aim assist, even with an icon-family override. Fresh controller activity selects controller input again. The additional 750 ms neutral-stick quarantine has been removed; aim integrators still clear on PC takeover and native mouse/melee processing remains intact. This input-ownership policy is not an anti-tamper mechanism.

An XInput wrapper cannot identify the physical controller behind it; use the `ps3` override for a PlayStation pad exposed as XInput. The native menu footer also selects embedded Xbox/PS3 images for SELECT/BACK rather than displaying textual button names. `ui_controllerIconFamily` is a read-only resolved-family setting used by Scaleform; change `ui_controllerIcons` to override it. Native low-ammo warnings use the restored weaponinfo ownerdraw item and QoS's existing ammo, reload-state and localization logic.

Controls > CONTROLLER now opens the original console settings timeline, with the earlier PC settings retained under ADVANCED CONTROLLER. Ship the entire 13-movie `consolation/scaleform/` bundle, including `cslcontroller.gfx` and `cslcontrolleradvanced.gfx`. `tools/patch_controller_frontend.py` adds these screens and footer artwork to the diagram-adapted bundle without building the client. Runtime validation remains required: test both icon families, footer input switching, reload/use, jump, melee, manual rebinding, unplug/reconnect and renderer restart.

## Analog Movement and Stance

The stock QoS protocol 47 transmits movement directions, not stick magnitude. Protocol 48 transmits both signed movement bytes through the verified QoS delta-command paths, retaining QoS-specific command fields. All clients and the server must run the matching patch, including keyboard-only clients. Stock protocol-47 peers are incompatible; the version gate rejects them rather than silently interpreting different command bits.

Controller movement applies the configured radial deadzone. Holds and menu-repeat timing continue across unchanged XInput packets or HID report gaps without stealing PC ownership. Saved custom bindings are preserved on startup; selecting a preset still applies that preset. Native `+stance` remains the default crouch/prone binding: tap crouches, hold enters prone using `cl_stanceHoldTime` (normally 300 ms), and press again while prone stands up. The patch leaves collision restrictions and native release handling intact.

Device discovery runs on a worker, publishing through the synchronized registry. Startup/hotplug requests do not run HID enumeration on the game thread. Driver binding and native input dispatch still occur on the main thread; opening a newly connected device can therefore still have a one-time cost.

## Known Limitations

- Device output capabilities do not imply all gameplay rumble or adaptive-trigger events are wired.
- Not every decorative Scaleform label is an input-binding prompt.
- SDL support is still early and currently focused on basic PlayStation-style controller detection/polling

## Planned Work

- Finish stabilizing the XInput implementation
- Finish stabilizing the SDL polling path
- Allow choosing backend (`XInput` vs `SDL`)
- Validate native PlayStation HID behavior, hotplug and output features across USB/Bluetooth devices.
