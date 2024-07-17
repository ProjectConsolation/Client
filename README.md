# Project: Consolation

A work-in-progress project aimed at extending and improving upon the PC port of [007: Quantum of Solace](https://en.wikipedia.org/wiki/007:_Quantum_of_Solace), a game that runs on the Call of Duty engine between <i>Call of Duty: 4</i> & <i>Call of Duty: World at War.</i>

 ## Current features
 - Fully working External Console
 - Various DVARs have been unlocked & edited from their defaults. (e.g [changes to movement to match IW3 movement closer](https://youtu.be/5LNlgfV1z-k), in-game fonts are HD now, etc)
 - Custom GSC script loading as well as overriding stock scripts
 - Borderless fullscreen
 - FPS unlocked
 - ``Probably more i forgot...``


## Custom Gun Game GSC Gameplay (Click to Play)
[![Custom GSC Gamemode Gameplay Video](https://img.youtube.com/vi/0Zu-5G9qdcg/maxresdefault.jpg)](https://www.youtube.com/watch?v=0Zu-5G9qdcg)


## Current issues
- Unable to debug much at all, due to the game having heavy anti debug
- "input" module for in-game console breaks mouse input completely
- Some RCE exploits may be present still
- dvarDump command's string formatting is broken, game finds empty dvar and crashes
- Certain localizations have different offsets despite being same game version (only known example so far is russian, read localization and add offset)
- ``Probably more i forgot...``

## Planned & started features
- In-Game console
- Re-add prone functionality
- Adding toggle to enable/disable cover, and prone (DVAR)
- Adding toggle for mantling without third person, and make it 1:1 to IW3 (DVAR)
- Adding toggle for climbing without third person, and make it 1:1 to IW3 (DVAR)
- Allow aiming to interrupt sprinting without letting go of any keys. (in other words, make it 1:1 to IW3)
- Force ``r_lodScale`` to ``0`` to prevent pop
- Patch ``com_maxfps``
- Patch overhead font for readability.
- Patch BG_GetPlayerMaxHealth to read DVAR & fix bot movement (Refer to other COD clients of the era)
- Custom Zones via [qos-xport](https://github.com/mjkzy/qos-xport/) & port Xbox/PS3/Wii maps & weapons that QOS PC didn't get.
- Replace scaleform in favor of .menu files for easy cross compatibility with COD assets
- Patch mouse input to get real raw mouse input
- Steam proxy, so it would show you're playing ``Project: Consolation`` on your friend's Steam friendslist.
- ``Probably more i forgot...``

# Credits 

 - [mjkzy](https://github.com/mjkzy) for working on zones & zonetool, finishing GSC support & providing a base + patches, upkeep
 - [Lierrmm](https://github.com/Lierrmm) for zones & zonetool, upkeep, patches
 - [JerryALT](https://github.com/JerryALT) for lots of code snippets from [IW3SP-MOD](https://github.com/JerryALT/iw3sp_mod)
 - [xoxor4d](https://github.com/xoxor4d) for another big lot of code snippets from [iw3xo](https://github.com/xoxor4d/iw3xo-dev)
 - [Rackover](https://github.com/Rackover) for IW3/IW4 research & [iw3x-port](https://github.com/iw4x/iw3x-port)
 - [MrReeko](https://github.com/MrReekoFTWxD) for initial GSC injection
 - [ujicos](https://github.com/ujicos) for initially starting the project, with additions & general upkeep

## Fun facts
- QoS is abandonware!
- QoS was drastically different on the Wii version and even recieved updates
- QoS seems to be <i><b>very</i></b> similar to the IW3 engine, even though it was developed by [a small team at] Treyarch during <i>World at War</i>
- the game had a very messy development process, and their deadline was 9 months (source: [Treyarch's 007 Quantum of Solace tie-in game](https://www.youtube.com/watch?v=bU4FkHVYYdU))
- the developers had to improvise 70% of the plot of the Singleplayer based off their own research into 007
- we could call this "T3", but it's not really a COD game so it wouldn't make much sense and is just a standalone Treyarch title between IW3 & T4
