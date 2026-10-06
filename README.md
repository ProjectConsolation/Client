# Project: Consolation

A work-in-progress project aimed at extending and improving the multiplayer PC port experience of [007: Quantum of Solace](https://en.wikipedia.org/wiki/007:_Quantum_of_Solace), a game that sits between <i>Call of Duty 4</i> (IW3) and <i>Call of Duty: World at War</i> (T4) on the Call of Duty engine.

Currently this has only been tested and confirmed to work on the English/French `1.1` version.

For installation help, launch setup, features, and common questions, use the wiki:

- [How do I obtain the game?](https://github.com/ProjectConsolation/Client/wiki#how-do-i-obtain-the-game)
- [Controller support](https://github.com/ProjectConsolation/Client/wiki/Controller-Support)
- [Crash debugging](https://github.com/ProjectConsolation/Client/wiki/Crash-Debugging)

## Nightly Builds

Nightly builds are automatically generated and available in the [Releases](https://github.com/ProjectConsolation/Client/releases) section. Open the latest nightly release and download `consolation-nightly.zip` from its Assets list.

These builds are **experimental and primarily intended for testing**. They are very likely to be unstable, partially broken, or not working at all. If a nightly build happens to work for you, consider yourself lucky - it may break or become outdated very quickly.

Nightly builds are provided **strictly for testing purposes** and should not be considered stable releases. **No support will be provided for nightly builds.**

## How to Install & Launch

Obtain the game from **MyAbandonware**.

Only the **English/French 1.1** release is supported right now.

Unsupported setups currently include:

- repacks
- other regional releases
- unpatched `1.0`
- other game versions

Install the game normally.

The recommended install path is the default path:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

Then install the official **1.1 patch**.

Patch `1.1` may not install correctly if the game is not installed in the default `C:\` location. To avoid path issues, the recommended setup is the default install path plus the official `1.1` patch.

If you are using a nightly build or a release build:

- extract or copy the complete build into the game root, preserving folders
- overwrite everything when prompted
- launch using `Launch Consolation.bat`

The batch launcher includes the multiplayer, default deathmatch and skip-intro arguments, and starts in the game directory.

The optional replacement is shipped under `optional/offline/xlive.dll`, not at
the archive root. Only copy it into the game root for offline/debug use, and add `-offline`
to the launch arguments. Leave it out for normal GFWL use. See
[the offline/debug DLL note](required_files/README_XLIVE.txt).
`generate-nightly.bat` generates both projects; building `d3d9` also builds the
optional shim and copies that note beside it. The shim is not auto-deployed.

Default launch command:

```bat
"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1
```

Nightly builds ship `Launch Consolation.bat` instead of the old `.lnk` file.

If your game is installed in the default directory, use the included batch file as-is.

If installed elsewhere, right-click `Launch Consolation.bat`, choose **Edit**, and change `GAME_DIR` to your real install directory.

For **offline/debug only**, copy `optional/offline/xlive.dll` into the game root and append `-offline -seta name "CSL_User00"` to the batch file's `start` command. Replace `CSL_User00` with your player name, keeping the quotes. Normal GFWL play should omit both the root-level replacement DLL and `-offline`. If an older nightly already installed the replacement in the game root, move that copy out; extracting the new archive does not remove it. Do not delete the system GFWL runtime.

`consolation/zone/common_consolation.ff` is a required client asset file. Keep it
at that location. The nightly packaging check rejects missing launchers or HUD
assets before publishing.

For a desktop launcher with the Consolation icon, create a shortcut to the `.bat`, then use **Properties → Shortcut → Change Icon → Browse** and select the bundled `icon.ico` in the game directory. Batch files have a generic Windows icon; changing the running game's icon does not update a shortcut to the original launcher.

Advanced users can still make a non-default install work by manually copying the patch `1.1` files into the game root and then correcting `GAME_DIR` in `Launch Consolation.bat`.

# Credits

- [mjkzy](https://github.com/mjkzy) for working on zones & zonetool, finishing GSC support & providing a base + patches, upkeep
- [Lierrmm](https://github.com/Lierrmm) for zones & zonetool, upkeep, patches
- [JerryALT](https://github.com/JerryALT) for lots of code snippets from [IW3SP-MOD](https://github.com/JerryALT/iw3sp_mod)
- [xoxor4d](https://github.com/xoxor4d) for another big lot of code snippets from [iw3xo](https://github.com/xoxor4d/iw3xo-dev)
- [JerryALT](https://gitea.com/JerryALT/) for IW3SP code snippets and research, raw mouse input
- [Rackover](https://github.com/Rackover) for IW3/IW4 research & [iw3x-port](https://github.com/iw4x/iw3x-port)
- [Wroyca](https://github.com/wroyca) for controller support and writing a dedicated custom DualSense driver
- [not-czar](https://github.com/not-czar) for the controller menu acceleration patch
- [MrReeko](https://github.com/MrReekoFTWxD) for initial GSC injection
- [ujicos](https://github.com/ujicos) for initially starting the project, with additions & general upkeep

## Fun Facts

- QoS is abandonware!
- QoS was drastically different on the Wii version and even received updates
- QoS seems to be <i><b>very</b></i> similar to the IW3 engine, even though it was developed by a small team at Treyarch during <i>World at War</i>, though structs seem to be a mix of 3arc and IW style.
- The game had a very messy development process, and their deadline was 9 months (source: [Treyarch's 007 Quantum of Solace tie-in game](https://www.youtube.com/watch?v=bU4FkHVYYdU))
- The developers had to improvise 70% of the plot of the singleplayer based off their own research into 007
