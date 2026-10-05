# ConsolationClient Working Instructions

## Session Startup

At the beginning of a new task:

1. Read this file and the applicable generated `AGENTS.md` files in full.
2. Check `git status --short --branch` and recent commits to understand the current workspace without changing it.
3. Treat the current files, code, runtime state, and fresh tool output as authoritative. Historical chat summaries and research notes may be stale.
4. Preserve unrelated user changes and work with them when they overlap the task.

## Project Goal

Partially reverse engineer *007: Quantum of Solace* multiplayer and extend it toward a complete, maintainable game client. The immediate direction is to support debugger attachment, replace Games for Windows LIVE with a local emulation layer, remove mandatory online dependencies, support persistent offline profiles, enter maps reliably, and establish a sound foundation for custom matchmaking.

Fixed success responses, hard-coded game state, manual snapshots, and similar shortcuts may be used only as explicitly identified probes. They must not be presented as completed implementations.

## Current Offline Objective

- Implement a complete local offline mode without requiring the original system `xlive.dll`.
- Link the offline username to the engine `name` dvar.
- Generate a stable, unique XUID for each local identity.
- Re-create required GFWL profile, settings, statistics, session, and title-storage behavior under `root\storage`.
- Preserve compatibility with the QoS title ID `0x41560829` and observed QoS serialization behavior.
- Study the original matchmaking boundaries and behavior so offline support can evolve into custom matchmaking.
- Keep control-plane, player-persistence, and authoritative-gameplay responsibilities logically distinct even when an implementation phase places them in one process.

## Analysis Targets and References

The primary PC analysis target is `jb_mp_s.dll`. Its IDA image base convention is `0x10000000`; preserve the repository's existing fixed-address and rebasing conventions.

In this project, a request to check "COD4" means the local [KisakCOD](https://github.com/SwagSoftware/KisakCOD) source tree, and "BO1" means the local [KisakBlack](https://github.com/SwagSoftware/KisakBlack) source tree. The intended sibling checkouts are `C:\Users\vamp\source\repos\KisakCOD` and `C:\Users\vamp\source\repos\KisakBlack`. Do not interpret those shorthand names as requests for the older COD4 or BO1 PDB/IDB databases.

For reference-dependent implementation work, the required pair is the QoS PC `jb_mp_s.dll` database open in IDA and the local KisakCOD source. KisakBlack is an additional comparative source when relevant. The following databases are optional historical references, not prerequisites:

- QoS Wii: `G:\DBs\QoS\jb_mp_final.plf.i64`
- COD4 alpha, Xbox 360: `G:\DBs\COD4A\_PDBLoaded.i64`
- COD4, macOS: `G:\DBs\MAC\Call of Duty 4 Multiplayer.i64`
- World at War, Xbox 360: `G:\DBs\WaW360.idb`
- Black Ops 1 server, PC: `G:\DBs\PC\BO1\CoDMPServer.pdb`

The COD4 alpha Xbox 360 database may appear in MCP as `G:\DBs\COD4A_PDBLoaded.i64`; this is the same COD4A reference as `G:\DBs\COD4A\_PDBLoaded.i64` under an alternate filename/path. Treat those names as aliases when MCP reports the former.

For safe-area comparisons, COD4A `ScrPlace_CalcSafeAreaOffsets` (`0x82173A18`) populates real and virtual viewable bounds from `safeArea_horizontal` and `safeArea_vertical`; HUD alignment consumes those bounds. In the QoS PC `jb_mp_s.dll` IDB, `scrPlaceView` is at `0x1127BA50`, and the screen-placement alignment helper is at `0x1030DF40`. That QoS build has no native `safeArea_horizontal`/`safeArea_vertical` strings, so the client registers the dvars and updates only the HUD placement bounds. Never apply safe-area offsets to the Consolation watermark or version overlay.

The installed Games for Windows LIVE SDK is mandatory for all XLive/GFWL reverse engineering:

- GFWL SDK: `C:\Program Files (x86)\Microsoft Games for Windows - LIVE SDK`

Inspect its headers, import libraries, samples, and documentation before implementing or naming XLive behavior. Do not invent XLive signatures, structures, constants, ordinals, or calling conventions when the SDK supplies them. QoS runtime evidence remains authoritative for game-specific behavior.

The QoS PC database and runtime evidence are authoritative for identifying functions and behavior. KisakCOD is a comparative source for inherited engine architecture, algorithms, ownership boundaries, and subsystem behavior; it is not a directly bindable implementation. The QoS Wii and Xbox 360 references remain useful when available but are not required for every investigation.

When a QoS name is unavailable or cannot be matched confidently, consult KisakCOD as a provisional naming or architectural reference, then validate the result against the QoS PC binary and runtime evidence before adopting it.

When KisakBlack material is relevant, use it only as an additional comparative source. Do not treat it as authoritative QoS behavior.

## Reverse-Engineering Standard

QoS evidence is authoritative. Prefer evidence in this order:

1. QoS xrefs, decompilation, runtime tracing, wire bytes, serialized data, and repeatable tests.
2. KisakCOD source, checked against QoS PC behavior.
3. Optional QoS Wii/Xbox references when they are available and relevant.
4. KisakBlack and other comparative implementations or optional databases when relevant.

Do not override QoS evidence because another title has a convenient symbol name or implementation. QoS lies between related engine generations but may retain, remove, or alter behavior independently.

Before transferring a function or variable name from another platform or title, validate the match with as many of the following as apply:

- callers and callees;
- xrefs and accessed globals;
- constants, strings, and structure offsets;
- argument use and return behavior;
- control-flow and algorithm shape;
- subsystem ownership and surrounding named functions;
- dynamic traces, wire data, or serialized output.

Do not use string similarity alone as proof. Account for platform ABI differences, compiler transformations, inlining, split or merged functions, endian differences, and code that exists on only one platform. Mark uncertain mappings as provisional and record the evidence and confidence instead of asserting an exact restoration.

When a relevant mandatory reference cannot be checked, state that verification is incomplete. Do not claim the implementation or name has been restored natively.

### Reference Availability Gate

- Before attempting a patch for functionality likely inherited from Call of Duty, such as windowed mode, renderer setup, window creation, or input behavior, inspect the relevant KisakCOD source and the QoS PC `jb_mp_s.dll` IDA database first. Much of this functionality may already be implemented in KisakCOD even when it is missing, disabled, or obscured in QoS.
- Compare the QoS implementation against KisakCOD before designing the patch; consult KisakBlack only when it adds relevant evidence.
- If the IDA MCP is unreachable or connected to the wrong database, immediately notify the user that the QoS PC `jb_mp_s.dll` database must be accessible, and wait for the user's input before continuing that reference-dependent investigation or patch.
- Do not replace the required QoS PC and KisakCOD checks with guesses, a low-confidence BinDiff result, or analysis of an unrelated active database.

## Engineering Rules

- Prefer native source implementations and QoS' own functions, state, and serialization.
- Preserve exact x86 layouts, calling conventions, fixed addresses, and ownership rules.
- Preserve `xlive.def` ordinals and `NONAME` declarations; they are ABI-critical.
- Treat closed-source components as explicit compatibility boundaries until they are replaced.
- Keep naming close to traceable QoS, COD4, and World at War symbols, while prioritizing QoS evidence and clarity in the current code.
- Temporary hooks, binary patches, and synthetic probes must document their purpose, applicable game build, verification conditions, and intended removal path.
- Match verification effort to risk. Shared protocols, persistence, profile formats, and cross-module contracts need repeatable regression coverage.
- A successful return code or UI transition does not prove that the underlying side effects and state transitions are correct.
- Do not delete user data, external reference databases, or non-reproducible files. Verify scope and value before removing obsolete generated artifacts or duplicate repository documentation.
- Document every newly registered user-facing command in the appropriate `docs/wiki` page in the same change.
- When an external project provides a structure that is relevant and verified against the QoS PC build, keep the required declaration locally with a concise source-attribution comment. Validate its layout with QoS evidence and `static_assert` checks where practical, avoid importing unverified fields wholesale, and tell the user which attributed definitions were added or adapted.
- When an update changes multiple related areas, provide both a concise Conventional Commit title (`fix:`, `feat:`, and so on) and a polished multi-paragraph summary suitable for the GitHub commit description. The description should explain the problem, the important implementation changes, and the verification performed without becoming a file-by-file changelog.

## Workspace Constraints

- Work only on source-code-related project material unless the user explicitly expands the scope.
- Do not modify editor settings, GitHub configuration, CI configuration, or unrelated external tooling.
- Do not overwrite or revert unrelated controller and gamepad work.
- Never build the project for the user.
- Never commit changes.
- When a game process is paused under Visual Studio and debugging is complete,
  close that game process when necessary to release files before installing an
  updated test zone. Do not launch the game for the user afterward.
