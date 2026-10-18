# Patched Raw Input

Project: Consolation includes a patched raw mouse input feature for the PC version of *007: Quantum of Solace*.

## Credits

This feature was adapted for *007: Quantum of Solace* from the raw mouse input work in IW3SP Mod by JerryALT and contributors:

- [IW3SP Mod RawMouse.cpp](https://gitea.com/JerryALT/iw3sp_mod/src/branch/main/src/Components/Modules/RawMouse.cpp)

## What It Does

- Adds a custom `m_rawInput` dvar
- Lets the client use Windows raw mouse input instead of relying entirely on the stock path
- Aims to improve mouse accuracy and polling behavior on modern systems

## Dvar

- `m_rawInput`
  - `0`: disabled
  - `1`: enabled

## Notes

- This feature is still experimental and may change as the mouse path is refined
- It is intended to preserve the stock engine flow as much as possible while improving mouse handling

## Alt-Tab and focus loss

The mouse hook releases the game's cursor confinement after native handling of
deactivation, focus loss and minimization, whether `m_rawInput` is enabled or
disabled. A foreground-window fallback also checks for missed focus messages.
It only removes a clip matching the game window and releases capture owned by
that window, rather than overriding another application's confinement.

Background raw mouse movement is discarded. Both accumulated movement and its
previous sample are reset when focus changes, preventing a queued camera jump
on return. The verified native mouse-active flag is cleared so the engine can
reacquire normally; controller settings and bindings are not changed.

Requires a rebuilt client DLL and full restart. Runtime validation remains:
Alt-Tab from menus and gameplay with raw input on/off, move onto another monitor,
then return and check cursor confinement, camera movement and controller cursor
visibility. Repeat while minimized. Source checks do not prove desktop behavior.

## Windowed menu cursor

In windowed mode, menus using native absolute mouse input no longer confine the
cursor to the game window. You can move outside and click another application
without Alt-Tab. Returning to gameplay invokes the native mouse activation path
to restore capture. Fullscreen, the native relative-input menu exception and
`cl_bypassMouseInput` retain their original behavior. This does not change the
display mode or require a new setting, and works with raw input on or off.

The menu clip hook is scoped to the verified QoS PC 1.1 activation call, with
instruction guards and shutdown restoration; it is not a global Windows API hook.
Rebuild the client and test menu entry/exit, clicks outside and back, gameplay
recapture, and controller-to-mouse switching before treating it as runtime verified.
