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
- `Esc`: cancel an explicitly selected autocomplete suggestion, otherwise close the console
- `Enter`: run exactly the typed command; if a suggestion was explicitly cycled, first commit it without executing
- `Tab`: autocomplete commands and dvars

## Navigation

- `Up` / `Down`: browse command history
- `Ctrl + P` / `Ctrl + N`: browse command history
- `Ctrl + Up` / `Ctrl + Down`: cycle autocomplete suggestions
- `Left` / `Right`: move the text cursor
- `Home` / `End`: jump to the start or end of the line
- `Ctrl + Home` / `Ctrl + End`: jump to the oldest/newest output

## Output Controls

- `Shift + Tab`: show or hide the output pane
- `Ctrl + Shift + Tab`: toggle fullscreen output pane
- `Shift + Up` / `Shift + Down`: scroll console output
- `PgUp` / `PgDn`: scroll console output
- Mouse wheel: scroll console output while the console is open
- `Ctrl + mouse wheel`: faster output scrolling
- `Shift + mouse wheel`: browse command history

## Editing Shortcuts

- `Backspace` / `Delete`: edit the current line
- Hold `Backspace`: repeat delete
- Held text and navigation keys repeat; keys already held when opening do not become new text presses
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
- Commands may optionally begin with `/` or `\`, as in the native console
- Gameplay key releases still reach the engine while the console is open, so held movement/fire bindings can release normally

## Notes

- This is a custom console, not a verified 1:1 COD4 port. Enter/completion, command prefixes and key-release handling were compared with KisakCOD and QoS PC `Con_KeyEvent`/`CL_KeyEvent`. Branded rendering, clipboard shortcuts, substring suggestions and output-pane controls remain intentional extensions. Bare input runs commands even in-game rather than becoming chat.
- The custom console installs its hooks lazily after the real game window exists to avoid startup crashes.
- `Shift + Tab` is intended for normal output viewing.
- `Ctrl + Shift + Tab` is intended for a larger fullscreen-style console view.
- Copy uses `Shift + Ctrl + C` instead of plain `Ctrl + C` so the console does not take over the normal system shortcut. During development this made it much easier to keep clipboard-based screenshots and other normal desktop copy flows working.

