# Console

Project: Consolation includes a custom in-game console overlay with its own rendering, input handling, history, scrolling, autocomplete, and full-console mode.

## Credits

The in-game console reimplementation was ported from the AlterWare / XLabs / momo5502 style in-game console work and then adapted for *007: Quantum of Solace*:

- [AlterWare IW6 console implementation](https://git.alterware.dev/alterware/iw6-mod/src/branch/master/src/client/component/game_console.cpp)

## Example

The screenshot below shows the custom in-game console overlay:

![Console example](assets/console/console.png)

## Main Keys

- `F1`, `\`, `< >`: open or close the small console
- `Shift + \`, `Shift + < >`: open or close the full console
- `Esc`: close the console
- `Enter`: run the current command
- `Tab`: autocomplete commands and dvars

## Navigation

- `Up` / `Down`: browse command history
- `Left` / `Right`: move the text cursor
- `Home` / `End`: jump to the start or end of the line

## Output Controls

- `Shift + Tab`: show or hide the output pane
- `Ctrl + Shift + Tab`: toggle fullscreen output pane
- `Shift + Up` / `Shift + Down`: scroll console output
- `PgUp` / `PgDn`: scroll console output
- Mouse wheel: scroll console output while the console is open

## Editing Shortcuts

- `Backspace` / `Delete`: edit the current line
- Hold `Backspace`: repeat delete
- `Ctrl + Backspace`: clear the current input line
- `Ctrl + A`: select the current input line
- `Shift + Ctrl + C`: copy the selected input text
- `Ctrl + V`: paste clipboard text
- `Ctrl + L`: clear console output

## Features

- Branded prompt: `Project: Consolation <version> [hash] >`
- Full-console footer with build/version information
- Autocomplete for commands and dvars
- Single-dvar details for current value, default value, description, and domain
- Scrollable full-console match list for large autocomplete results
- Output mirrored through the game's console print path
- Basic clipboard editing support for the input line

## Notes

- This console was simple to port over nearly 1:1 compared to the original IW6 reimplementation, then adjusted for this game's renderer, input, and dvar environment.
- The custom console installs its hooks lazily after the real game window exists to avoid startup crashes.
- `Shift + Tab` is intended for normal output viewing.
- `Ctrl + Shift + Tab` is intended for a larger fullscreen-style console view.
- Copy uses `Shift + Ctrl + C` instead of plain `Ctrl + C` so the console does not take over the normal system shortcut. During development this made it much easier to keep clipboard-based screenshots and other normal desktop copy flows working.

