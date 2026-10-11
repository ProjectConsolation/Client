OPTIONAL, HIGHLY RECOMMENDED LOCAL GFWL / LAN DLL

optional\offline\xlive.dll is Project: Consolation's local Games for Windows
LIVE replacement. It is optional for the client for now, but required on BOTH
PCs for Consolation's GFWL emulation and LAN transport. It is highly recommended.
It does not connect to Microsoft's original online GFWL service.

Close the game, back up any existing game-root xlive.dll, then copy
optional\offline\xlive.dll beside JB_Launcher_s.exe in the game ROOT.
Launch normally with Launch Consolation.bat, or:
JB_Launcher_s.exe -multiplayer

The client detects the installed shim automatically. Normal two-PC LAN play
does NOT require -offline, -offline_test, -offline_test_port or offlineNetStatus.
Both PCs may use the normal UDP 1000/1001 ports because they have separate IPs.
Host a match through SYSTEM LINK. Join through its browser, or use
connectIP <host-IP>:1000. Do not use 127.0.0.1 to reach another PC.
Use matching client/shim versions and matching map files on both PCs.

CURRENT IDENTITY LIMITATION: use different startup player names on both PCs.
Normal offline XUIDs still derive from the startup name; identical default
names may collide. If necessary, add -name "YourName" to your launcher arguments.
Changing the startup name can select another profile; keep it consistent.
The isolated -offline_test switches remain available for developer diagnostics
and two game instances on one PC, not as normal nightly setup requirements.

For normal GFWL use, do not copy this optional DLL into the game directory.
If you already installed this project's replacement there, move that local
copy out before using normal GFWL. Do not remove the original system DLL.

Nightly archives keep this DLL in optional\offline so extracting the complete
archive will not replace normal GFWL operation. An older offline replacement
already in the game root is not removed automatically; move it out explicitly
before switching back to normal GFWL. Never delete the system runtime.

Building the client produces both DLLs. Only d3d9.dll is automatically deployed;
the optional local shim must be installed explicitly.

Offline profile and title-storage data are kept under the game storage folder.
