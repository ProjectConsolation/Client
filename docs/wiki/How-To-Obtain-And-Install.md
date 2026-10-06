# How To Obtain And Install

## How Do I Obtain The Game?

Obtain **007: Quantum of Solace** from **MyAbandonware**.

This is currently the recommended source because it hosts the **English/French build** that works with Project: Consolation and also hosts the **1.1 patch** you need to install.

Only the **English/French 1.1** release is supported right now.

Unsupported setups currently include:

- repacks
- other regional releases
- unpatched `1.0`
- other game versions

Games for Windows - LIVE keys are also often given away in our Discord:

- [Project: Consolation Discord](https://discord.gg/XSrTvXJcsw)

Work is also being done to remove the long-term need for keys entirely, but that is not ready yet.

## Quantum of Solace On PC

*007: Quantum of Solace* on PC has effectively been unavailable through normal first-hand digital storefronts for a long time.

That means people usually end up getting the game through second-hand copies, key resellers, or abandonware archives rather than through an official current storefront.

Availability and legality can vary by region, so use your own judgment and follow local rules.

## Games for Windows - LIVE

You should install Games for Windows - LIVE separately using an up-to-date package.

The recommended source is:

- [PCGamingWiki Community: Microsoft Games for Windows - LIVE](https://community.pcgamingwiki.com/files/file/1012-microsoft-games-for-windows-live/)

Keep in mind that newer Games for Windows - LIVE packages may behave differently over time depending on compatibility changes.

## How Do I Install And Launch It?

Install the game normally.

The recommended install path is the default path:

- `C:\Program Files (x86)\Activision\Quantum of Solace(TM)\`

Then install the official **1.1 patch**.

Patch `1.1` may not install correctly if the game is not installed in the default `C:\` location. To avoid path issues, the recommended setup is the default install path plus the official `1.1` patch.

If you are using a nightly build or a release build:

- download `consolation-nightly.zip` from the Assets list of the latest nightly in [Releases](https://github.com/ProjectConsolation/Client/releases)
- extract or copy the build into the game root, excluding the optional `xlive.dll` unless using offline/debug mode
- overwrite everything when prompted
- launch using `Launch Consolation.bat`

The batch launcher sets the game directory as its working directory and uses this default command:

```bat
"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1
```

Nightlies include `Launch Consolation.bat` instead of the old `.lnk` shortcut.

If your game is installed in the default directory, use the batch file as-is.

If installed elsewhere, right-click `Launch Consolation.bat`, choose **Edit**, and change `GAME_DIR` to your real install directory.

### Optional offline/debug launch and player name

For offline/debug use, copy the bundled `xlive.dll` into the game root and append this to the `start` command in `Launch Consolation.bat`:

```text
-offline -seta name "CSL_User00"
```

Replace `CSL_User00` with your desired player name, keeping the quotation marks. The full offline command is:

```bat
"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 -offline -seta name "CSL_User00"
```

For normal GFWL play, leave out the optional `xlive.dll` and `-offline`. Offline mode does not provide online GFWL service emulation.

### Desktop shortcut icon

Create a desktop shortcut to `Launch Consolation.bat`. Open its **Properties → Shortcut → Change Icon → Browse** and select the bundled `icon.ico` from the game directory. A `.bat` has a generic Windows icon by default; the icon applied to the running game does not automatically change a shortcut to `JB_Launcher_s.exe`.

Advanced users can still use another install directory by manually copying the patch `1.1` files into the game root and correcting `GAME_DIR` in the batch file.
