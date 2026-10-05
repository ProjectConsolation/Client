OPTIONAL OFFLINE / DEBUG DLL

Only use the included xlive.dll if you want to run offline or debug using
Project: Consolation's local Games for Windows LIVE replacement.
It is not a replacement for normal online GFWL operation.

For offline/debug use, copy xlive.dll beside JB_Launcher_s.exe and launch:
JB_Launcher_s.exe -multiplayer -offline

For normal GFWL use, do not copy this optional DLL into the game directory.
If you already installed this project's replacement there, move that local
copy out before using normal GFWL. Do not remove the original system DLL.

Building the client produces both d3d9.dll and xlive.dll, with this note beside
them. Only d3d9.dll is automatically deployed to the configured game directory;
the offline/debug shim must be installed explicitly.

Offline profile and title-storage data are kept under the game storage folder.
