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
- `resetcustomres`
- `dumpultrawide`

Example:

`setcustomres 1920x720`

## Notes

- `setcustomres` applies a custom resolution, updates the custom ultrawide aspect ratio, and restarts the renderer.
- `resetcustomres` disables the ultrawide override and returns the game to the default custom-resolution path.
- `dumpultrawide` prints the live ultrawide state for debugging.

## How To Disable Ultrawide

Run:

`resetcustomres`

This disables the ultrawide override and restarts the renderer so the game returns to its normal behavior.

## If You Run Into Issues

If the game starts stretched, menus look wrong, or the in-game view does not update correctly after startup, run `setcustomres` again after the game has fully loaded or after joining a match.

If that does not help, return to a normal setup with `resetcustomres` and try a different ultrawide resolution later.
