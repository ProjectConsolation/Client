# Commands

Project: Consolation includes a small set of utility and debugging commands that are useful for advanced users, modders, and troubleshooting.

## Commands

| Name | Description | Example |
| :--- | :--- | :--- |
| `addbot [count]` | Spawns one or more multiplayer bots and assigns names from `consolation/bots.txt` when available. | `addbot` `addbot 4` |
| `dvarDump [filename]` | Prints all registered dvars to the console and can optionally write them to a text file under the `consolation` folder. | `dvarDump` `dvarDump dvars` |
| `commandDump [filename]` | Prints all registered command names to the console and can optionally write them to a text file under the `consolation` folder. | `commandDump` `commandDump commands` |
| `listassetpool <poolnumber> [filter]` | Lists assets from the selected asset pool and can optionally filter the output by text. | `listassetpool 0` `listassetpool 13 weapon` |
| `origin [0|1]` | Debug builds only. Toggles a twice-per-second origin and velocity status line in the internal game console; an explicit value enables or disables it. | `origin` `origin 1` `origin 0` |

## Notes

- If you provide a filename to `dvarDump` or `commandDump`, `.txt` is added automatically if needed.
- `commandDump` is still rough and may produce mangled or incomplete results in some cases.
- `listassetpool` can also be run without arguments to print the available pool numbers and asset type names.
