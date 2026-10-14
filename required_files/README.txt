Use the included "Launch Consolation.bat" to start multiplayer.

Copy the complete nightly archive into the game directory, preserving folders.
Launch Consolation.bat checks the latest published GitHub client package before
starting. It downloads/caches the complete archive, verifies GitHub's SHA-256
digest and the package manifest, and compares installed file hashes.
Choose Yes to download verified files with backups under consolation-update-backups;
choose No to launch unchanged and check again next time. Network failures also
allow launch.
The scan includes DLLs and new/changed fastfiles (.ff) in the same release.
Playlists are embedded rawfiles in patch_consolation.ff; no loose playlists.info
is required or downloaded.
All downloads finish before files are replaced and the launcher waits for the
update to finish before starting the game. Client DLLs and assets stay matched.
Debug builds are detected from d3d9.dll's version-resource debug flag. Available
updates print "[updater] update available, but skipped due to debug build"
without prompting or modifying installed files. The launcher proceeds normally.
An already-installed offline shim is updated in the root only if it matches
the old bundled optional/offline/xlive.dll and contains the shim marker; retail
GFWL is never replaced implicitly. Profiles and user identity files are untouched.
Launching the EXE directly bypasses this check. Older releases without an update
manifest cannot be installed automatically. Protected game directories need write
permission; failures warn and preserve startup rather than silently elevating.
The bundled steam_api.dll is the Steamworks SDK's x86 redistributable and also
belongs beside JB_LiveEngine_s.exe. Packaging uses it when the build directory
does not contain a runtime copy. This does not enable Steam in an SDK-disabled
client build: the client must still be compiled with the Steamworks SDK.
companion64.dll belongs beside JB_LiveEngine_s.exe. It runs inside native
64-bit Steam, not the x86 game, and supplies the Project Consolation activity
title. Keep licenses\companion\ with redistributed copies; matching source,
local Windows build fixes, and upstream license notices are bundled there.
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
