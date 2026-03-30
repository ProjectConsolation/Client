# Console

Project: Consolation includes a custom in-game console overlay with its own rendering, input handling, history, scrolling, autocomplete, and full-console mode.

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
- `Ctrl + V`: paste clipboard text
- `Ctrl + L`: clear console output

## Features

- Branded prompt: `Project: Consolation <version> [hash] >`
- Full-console footer with build/version information
- Autocomplete for commands and dvars
- Single-dvar details for current value, default value, description, and domain
- Scrollable full-console match list for large autocomplete results
- Output mirrored through the game's console print path

## Notes

- The custom console installs its hooks lazily after the real game window exists to avoid startup crashes.
- `Shift + Tab` is intended for normal output viewing.
- `Ctrl + Shift + Tab` is intended for a larger fullscreen-style console view.
