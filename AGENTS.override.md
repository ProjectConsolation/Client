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

Use these loaded IDA databases as symbol and implementation references:

- QoS Wii: `G:\DBs\QoS\jb_mp_final.plf.i64`
- COD4 alpha, Xbox 360: `G:\DBs\COD4A\_PDBLoaded.i64`
- COD4, macOS: `G:\DBs\MAC\Call of Duty 4 Multiplayer.i64`
- World at War, Xbox 360: `G:\DBs\WaW360.idb`

Use the installed Games for Windows LIVE SDK to validate public APIs, constants, structures, calling conventions, and expected behavior:

- GFWL SDK: `C:\Program Files (x86)\Microsoft Games for Windows - LIVE SDK`

The QoS Wii database is the primary naming reference for identifying and renaming corresponding functions in the PC `jb_mp_s.dll` IDB. COD4 and World at War databases are comparative references for inherited engine architecture, algorithms, ownership boundaries, and subsystem behavior. They are not directly bindable implementations.

When BO1 Windows x86 server or KisakBlack material is available and relevant, use it only as an additional comparative source, especially where COD4 and World at War differ. Do not treat it as authoritative QoS behavior.

## Reverse-Engineering Standard

QoS evidence is authoritative. Prefer evidence in this order:

1. QoS xrefs, decompilation, runtime tracing, wire bytes, serialized data, and repeatable tests.
2. The QoS Wii symbol database and corresponding Wii implementation context.
3. Comparative COD4 and World at War databases, checked together when relevant.
4. BO1/KisakBlack and other community reference implementations.

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

## Workspace Constraints

- Work only on source-code-related project material unless the user explicitly expands the scope.
- Do not modify editor settings, GitHub configuration, CI configuration, or unrelated external tooling.
- Do not overwrite or revert unrelated controller and gamepad work.
- Never build the project for the user.
- Never commit changes.
