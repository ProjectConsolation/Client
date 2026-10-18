# Project: Consolation

A work-in-progress project aimed at extending and improving the multiplayer PC port experience of [007: Quantum of Solace](https://en.wikipedia.org/wiki/007:_Quantum_of_Solace), a game that sits between <i>Call of Duty 4</i> (IW3) and <i>Call of Duty: World at War</i> (T4) on the Call of Duty engine.

Currently this has only been tested and confirmed to work on the English/French `1.1` version.

For installation help, launch setup, features, and common questions, use the wiki:

- [How do I obtain the game?](https://github.com/ProjectConsolation/Client/wiki#how-do-i-obtain-the-game)
- [Controller support](https://github.com/ProjectConsolation/Client/wiki/Controller-Support)
- [Crash debugging](https://github.com/ProjectConsolation/Client/wiki/Crash-Debugging)

An experimental native C# desktop launcher is available as a separate
[source subproject](src/launcher/README.md). It uses WPF rather than CEF and is
not yet included in nightly packages; the batch launcher remains the normal
packaged entry point.

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

The bundled `xlive.dll` is required for both Steam and non-Steam play. It replaces
the game's Games for Windows LIVE dependency and ships at the archive root,
beside `d3d9.dll`. Keep both DLLs in the game root and update them together.
Launch normally: detection is automatic, with no `-offline` or `-offline_test`
requirement. Do not delete or replace the Windows system GFWL runtime.
See [the required XLive replacement note](required_files/README_XLIVE.txt).
`generate-nightly.bat` generates both projects; building `d3d9` also builds the
required shim and copies that note beside it.

Default launch command:

```bat
"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1
```

Nightly builds ship `Launch Consolation.bat` instead of the old `.lnk` file.

If your game is installed in the default directory, use the included batch file as-is.

If installed elsewhere, right-click `Launch Consolation.bat`, choose **Edit**, and change `GAME_DIR` to your real install directory.

For online multiplayer, start Steam, sign in, then launch Consolation and use
PLAY ONLINE. Steam matchmaking and peer-to-peer connections do not require
setting up a LAN connection or using the original GFWL service.

If you do not use Steam, LAN play remains available as a fallback. Install the
same client files, including the required `xlive.dll`, on both PCs and use
SYSTEM LINK to host and join. Direct joins use `connectIP <host-IP>:1000`.
Normal two-PC LAN play uses
UDP 1000/1001 on each PC; debug port overrides are only needed for special tests.
Offline identities persist independently of player names. Do not copy another
PC's identity files or profile storage when setting up a second installation.
The debug commands remain available but are not normal launch requirements.
When upgrading from an older nightly that placed the DLL under `optional/offline`,
use the new archive's root `xlive.dll`; the old optional-folder copy is not used.

`consolation/zone/common_consolation.ff` is a required client asset file. Keep it
at that location. The nightly packaging check rejects missing launchers or HUD
assets before publishing.

For a desktop launcher with the Consolation icon, create a shortcut to the `.bat`, then use **Properties → Shortcut → Change Icon → Browse** and select the bundled `icon.ico` in the game directory. Batch files have a generic Windows icon; changing the running game's icon does not update a shortcut to the original launcher.

Advanced users can still make a non-default install work by manually copying the patch `1.1` files into the game root and then correcting `GAME_DIR` in `Launch Consolation.bat`.

# Credits

- [mjkzy](https://github.com/mjkzy) for working on zones & zonetool, finishing GSC support & providing a base + patches, upkeep
- [Lierrmm](https://github.com/Lierrmm) for zones & zonetool, upkeep, patches
- [mo (michaeloliverx)](https://github.com/michaeloliverx) for zone help and expanding OpenAssetTools support
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
