Use the included "Launch Consolation.bat" to start multiplayer.

Copy the complete nightly archive into the game directory, preserving folders.
consolation\zone\common_consolation.ff is REQUIRED for the client HUD assets;
do not move it into the root directory or omit it when updating.

consolation\scaleform\ contains REQUIRED native menu overrides, including
pcsharedlibrary.gfx and cmsharedplatform.gfx. Copy all six .gfx files together; omitting a shared
library can leave Single Player and the old video settings visible.
These modify the stock menus, not the experimental custom frontend.
Fully close and restart the game after updating; a DLL rebuild alone does
not regenerate or install these assets. Original main\scaleform files stay intact.

Only install optional\offline\xlive.dll for offline/debug use. Leave it there for
normal Games for Windows LIVE operation. See README_XLIVE.txt for instructions.

Default target:
"C:\Program Files (x86)\Activision\Quantum of Solace(TM)\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1

If installed elsewhere, right-click Launch Consolation.bat, choose Edit,
and change GAME_DIR to your game directory.

OFFLINE / DEBUG ONLY
Copy optional\offline\xlive.dll into the game root and append the following to the START command
in Launch Consolation.bat:
-offline -seta name "CSL_User00"
Replace CSL_User00 with your own player name; keep the quotes.
Do not add -offline or install the bundled xlive.dll for normal GFWL play.

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
