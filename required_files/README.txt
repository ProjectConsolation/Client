Use the included "Launch Consolation.bat" to start multiplayer.

Copy the complete nightly archive into the game directory, preserving folders.
consolation\zone\common_consolation.ff is REQUIRED for the client HUD assets;
do not move it into the root directory or omit it when updating.

consolation\scaleform\ contains REQUIRED native menu overrides, including
cmfrontend.gfx, pcsharedlibrary.gfx and cmsharedplatform.gfx. Copy all bundled
.gfx files together. The frontend bootstrap constructs the menus before the
screen movies load; omitting it leaves Single Player and old video settings.
These modify the stock menus, not the experimental custom frontend.
Fully close and restart the game after updating; a DLL rebuild alone does
not regenerate or install these assets. Original main\scaleform files stay intact.

optional\offline\xlive.dll is optional for now, but highly recommended and
REQUIRED on both PCs for Consolation's local GFWL emulation and LAN transport.
Install it explicitly in the game root. See README_XLIVE.txt for instructions.

Default target:
"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1

If installed elsewhere, right-click Launch Consolation.bat, choose Edit,
and change GAME_DIR to your game directory.

LOCAL GFWL / LAN
Copy optional\offline\xlive.dll into the game root and launch normally.
No -offline or -offline_test flags are needed for ordinary two-PC play.
Host and join through SYSTEM LINK, or join with connectIP <host-IP>:1000.
Use different startup player names: current name-derived XUIDs can collide if
both PCs use the same name. See README_XLIVE.txt for details and debug options.
For Microsoft's original GFWL runtime, leave the optional DLL uninstalled.

DESKTOP ICON
Windows batch files use a generic icon. Run Repair Consolation Shortcut.ps1
with PowerShell to repair an existing pinned "Launch Consolation" shortcut.
The helper preserves your arguments, backs up the shortcut, sets icon.ico,
and assigns the same taskbar application identity as the client window.
To create a desktop shortcut instead, pass -ShortcutPath with its full .lnk path.
If Windows retains an old cached icon, unpin and repin the repaired shortcut.
The client update needs rebuilding before the matching window identity takes effect.

Patch 1.1 may not install correctly outside the default C:\ install path.
Recommended: install to the default directory.
Advanced users can manually copy the patch 1.1 files into the game root and then fix GAME_DIR.
