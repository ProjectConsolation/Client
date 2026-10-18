REQUIRED XLIVE REPLACEMENT - STEAM AND NON-STEAM PLAY

xlive.dll is Project: Consolation's local Games for Windows LIVE replacement.
It is required for the client, including Steam matchmaking and non-Steam LAN play.
It does not connect to Microsoft's original online GFWL service.

Close the game, back up any existing game-root xlive.dll, then copy
the bundled root xlive.dll beside JB_Launcher_s.exe in the game ROOT.
Launch normally with Launch Consolation.bat, or:
JB_Launcher_s.exe -multiplayer

The client detects the installed shim automatically. For online play, start
Steam and sign in before launching, then use PLAY ONLINE. LAN setup is not
needed for Steam matchmaking and peer-to-peer connections.

If you do not use Steam, SYSTEM LINK remains available as a LAN fallback.
Normal two-PC LAN play
does NOT require -offline, -offline_test, -offline_test_port or offlineNetStatus.
Both PCs may use the normal UDP 1000/1001 ports because they have separate IPs.
Host a match through SYSTEM LINK. Join through its browser, or use
connectIP <host-IP>:1000. Do not use 127.0.0.1 to reach another PC.
Use matching client/shim versions and matching map files on both PCs.

Normal offline identities persist under
%LOCALAPPDATA%\ConsolationClient\offline-identities and do not depend on your
name or hardware. Two PCs may use identical player names without deriving
the same identity. Do not copy or distribute these identity files with a build.
Changing your name keeps the identity and selected profile. Existing profile
storage is retained on first use; back up storage and the identity files together.
The isolated -offline_test switches remain available for developer diagnostics
and two game instances on one PC, not as normal nightly setup requirements.

Nightly archives ship this DLL at the root alongside d3d9.dll. Extract the
complete archive into the game directory and update both DLLs together.
Older archives kept it in optional\offline; use the new root copy instead.
Do not remove or overwrite the original Windows system GFWL DLL.

Building the client produces both DLLs; install both build outputs together.

Offline profile and title-storage data are kept under the game storage folder.
