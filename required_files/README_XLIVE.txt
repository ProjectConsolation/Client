OPTIONAL OFFLINE / DEBUG DLL

Only use optional\offline\xlive.dll if you want to run offline or debug using
Project: Consolation's local Games for Windows LIVE replacement.
It is not a replacement for normal online GFWL operation.

For offline/debug use, copy optional\offline\xlive.dll beside JB_Launcher_s.exe
in the game ROOT (not inside the optional folder), then launch:
JB_Launcher_s.exe -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 -offline -seta name "CSL_User00"

In Launch Consolation.bat, append -offline -seta name "CSL_User00" to the
START command. Replace CSL_User00 with your own name, keeping the quotes.
The -offline flag alone does not install or replace the GFWL runtime.

For normal GFWL use, do not copy this optional DLL into the game directory.
If you already installed this project's replacement there, move that local
copy out before using normal GFWL. Do not remove the original system DLL.

Nightly archives keep this DLL in optional\offline so extracting the complete
archive will not replace normal GFWL operation. An older offline replacement
already in the game root is not removed automatically; move it out explicitly
before switching back to normal GFWL. Never delete the system runtime.

Building the client produces both DLLs. Only d3d9.dll is automatically deployed;
the offline/debug shim must be installed explicitly.

Offline profile and title-storage data are kept under the game storage folder.
