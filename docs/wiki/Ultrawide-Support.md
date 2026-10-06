# Ultrawide Support

Project: Consolation includes experimental ultrawide support for custom aspect ratios and custom render resolutions in *007: Quantum of Solace* multiplayer.

## Experimental Status

This feature is still experimental.

*007: Quantum of Solace* was not originally designed for ultrawide resolutions.

Because of that, some menus, UI elements, crosshairs, transitions, or other visual details may still behave inconsistently depending on the selected resolution and when the renderer is restarted.

Menus also currently seem a little broken in some cases, even when the in-game view itself is working correctly.

## Example

The screenshot below shows an example of ultrawide gameplay with the current experimental support:

![Ultrawide gameplay example](assets/ultrawide/ultrawide.png)

## Commands

- `setcustomres <width>x<height>`
- `clearcustomres`
- `dumpultrawide`

Example:

`setcustomres 1920x720`

## Notes

- `setcustomres` applies a custom resolution, updates the custom ultrawide aspect ratio, and restarts the renderer.
- `clearcustomres` disables the explicit override and clears the custom resolution. Native `r_aspectRatio Auto` still uses the actual display ratio on ultrawide displays.
- `dumpultrawide` prints the live ultrawide state for debugging.

## How To Disable Ultrawide

Run:

`clearcustomres`

This clears the explicit override and restarts the renderer. Choose a native `r_aspectRatio` preset to force 4:3, 16:10 or 16:9 instead of automatic ultrawide detection.

The aspect correction runs immediately after QoS stores its window settings, on renderer initialization and device reset. It updates the verified window and pixel-aspect fields before viewport/FOV setup; it does not apply HUD safe-area offsets to menus or overlays. This replaces the previously unused aspect updater and avoids writing console/input memory as an aspect scalar.

For a manual ratio, set `r_aspectRatioCustomEnable 1` and `r_aspectRatioCustom` to width divided by height, then run `vid_restart`. `setcustomres` performs these steps automatically. Disabling the override no longer gets undone by a saved `r_ultrawideCustomMode`, and changing ratios does not rewrite dvar defaults.

## If You Run Into Issues

If the game starts stretched, menus look wrong, or the in-game view does not update correctly after startup, run `setcustomres` again after the game has fully loaded or after joining a match.

If that does not help, run `clearcustomres`, select a native aspect preset, and record `dumpultrawide` output. Menu layouts still require in-game verification; this does not claim every Scaleform screen has been adapted to ultrawide.
