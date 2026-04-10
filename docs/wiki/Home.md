# Project: Consolation Wiki

Use this wiki for installation help, launch setup, controller notes, patched systems, and troubleshooting.

## How Do I Obtain The Game?

Obtain the game from **MyAbandonware**.

Only the **English/French 1.1** release is supported right now.

Unsupported setups currently include:

- repacks
- other regional releases
- unpatched `1.0`
- other game versions

## How Do I Install And Launch?

Install the game normally.

The recommended install path is the default path:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

Then install the official **1.1 patch**.

Patch `1.1` may not install correctly if the game is not installed in the default `C:\` location. To avoid path issues, the recommended setup is the default install path plus the official `1.1` patch.

If you are using a nightly build or a release build:

- extract or copy the build into the game root
- overwrite everything when prompted
- launch using `Launch Consolation.lnk`

The included shortcut already uses the required `-multiplayer` launch argument.

Example target behind the shortcut:

- `"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer`

Nightly builds should ship with `Launch Consolation.lnk` for this.

If your game is installed in the default directory, you should be able to use the included shortcut as-is.

If your game is not installed in the default directory, edit the shortcut target so it points at your real install path.

Advanced users can still make a non-default install work by manually copying the patch `1.1` files into the game root and then editing `Launch Consolation.lnk` so it points at the correct `JB_Launcher_s.exe` path.

## Extended Features

- [Extended Features](Extended-Features.md)
- [Bots](Bots.md)
- [Console](Console.md)
- [Commands](Commands.md)
- [Patched Dvars](Patched-Dvars.md)
- [Patched Raw Input](Patched-Raw-Input.md)
- [Profile Config Tools](Profile-Config-Tools.md)
- [Ultrawide Support](Ultrawide-Support.md)

## GSC

- [GSC Loading And Overrides](GSC-Loading-And-Overrides.md)

## Troubleshooting

- [Controller Support](ControllerSupport.md)
- [Crash Debugging](CrashDebugging.md)
