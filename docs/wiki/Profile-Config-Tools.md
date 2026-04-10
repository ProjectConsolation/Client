# Profile Config Tools

Project: Consolation includes a few helper commands for working with the game's new profile config files.

These are mainly useful for debugging, recovery, and converting the stock encrypted profile config into the custom plaintext Project: Consolation config workflow.

## Commands

### `profile_show_config_path`

Prints the currently active runtime config path.

This is useful when you want to know exactly which file the game is loading or saving.

### `profile_decrypt_config`

Reads the stock profile config when possible and writes a plaintext decrypted copy.

This is useful if you want to inspect the old config contents without editing the original encrypted profile file directly.

### `profile_convert_config`

Converts the active profile config into the custom `consolation_mp.cfg` workflow.

This is the main migration-style command for moving into Project: Consolation's custom runtime config.

### `profile_dedupe_config`

Cleans up duplicate dvar assignments in the custom runtime config.

This is helpful if repeated saves or manual edits have left the file noisy or inconsistent.

## Notes

- These commands are mostly for advanced users, testing, and troubleshooting.
- The active custom runtime config now lives in the profile-specific AppData path at `%AppData%/Activision/Quantum of Solace/players/<profile>/consolation_mp.cfg`.
- Runtime-only values that should not persist between launches may be stripped during cleanup to avoid bad startup state.
- Some commands may be duplicated with case insensitivity, this is being worked on to fix it.

## Portable Override (Advanced)

Project: Consolation can also mirror the custom config to a portable override under the game install folder.

Portable override path:

- `consolation/players/<profile>/consolation_mp.cfg`

Example:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\consolation\players\Default\consolation_mp.cfg`

How it works:

- AppData remains the live runtime config location.
- If the portable profile folder already exists, Project: Consolation will import the portable `consolation_mp.cfg` into the AppData runtime config on startup.
- On a clean shutdown, the current runtime config is mirrored back to the portable file.

Important:

- This is meant for advanced users only.
- It is not recommended as the default setup.
- If the game is installed under `Program Files`, Windows permissions may block writes to the portable file unless your setup allows it.
- If the portable override cannot be written, the AppData config remains the safe/default path.
