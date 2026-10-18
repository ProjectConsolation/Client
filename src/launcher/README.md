# Consolation Launcher

Independent C# WPF desktop subproject. Open `Consolation.Launcher.sln` directly,
or regenerate the normal Visual Studio solution to find it in the Launcher
group. It has no dependency on the C++ DLL build and does not deploy itself
into a game installation. The original launch scripts remain supported.

Requires the .NET 10 SDK and Windows desktop development tools to build; a
framework-dependent deployment requires the x86 .NET 10 Desktop Runtime.
Output goes under the repository's ignored `build/launcher` directory. Deploy
the complete launcher output together, not just the executable.

## Interface

An original WPF implementation inspired by the X Labs launcher's compact
navigation rail, rounded charcoal cards and coloured title-bar controls.
Original X Labs launcher and layout credit: [momo5502](https://github.com/momo5502).
No X Labs source, binaries, logos, artwork, CEF or WebView components are bundled.
The hero uses the supplied `jb.png` artwork with a declarative opacity fade,
the supplied wide Project: Consolation logo, and the supplied square sidebar
icon. These PNGs are embedded resources copied unchanged into `Assets/`, so the
launcher never depends on Downloads or an installed game for its branding.
The executable's Windows icon remains the existing Consolation `.ico`.

- **Play:** launch the supported English/French 1.1 multiplayer installation.
- **Settings:** select the game root, save it and choose pre-launch update checking.
  After starting the game, the desktop launcher exits completely rather than
  minimizing. Failed launches keep it open; update/repair actions do not exit it.
  Old minimize-after-launch preferences are ignored.
- **Install guide / Donate:** sidebar links to the Client installation wiki and
  `https://buymeacoffee.com/consolation`, opened only when clicked.
- **Community:** open the Client wiki/devlog, GitHub or Discord in the default
  browser. No remote content is fetched when the launcher opens.

Settings are stored at `%LOCALAPPDATA%/ProjectConsolation/launcher.json`, written
through a temporary file and atomic replacement. Explicit saved directories
take priority; otherwise discovery checks beside the launcher and then the
standard Activision install directory. The working directory is not used for
discovery. Game profile files and Steam credentials are never edited.

## Launch and update boundary

Play checks for `JB_Launcher_s.exe`, `jb_mp_s.dll`, `d3d9.dll`, the required root
`xlive.dll` and `consolation/zone/common_consolation.ff`. Presence is not a game
version/ABI validation; the client still performs its normal startup checks.
It starts the existing game launcher directly in the selected game directory:

```text
JB_Launcher_s.exe -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1
```

The launcher does not override the game's fullscreen/windowed setting. The
display-mode toggle has been removed; older saved values are ignored. Launch toggles:

| Setting | Arguments / behavior |
| --- | --- |
| Skip intro movies | `-set cin_firstRunDone 1 -set cin_skipAllMovies 1`; both become `0` when off |
| Check updates before play | Runs the existing updater; when off, skips it and adds `-noupdate` |
| Asset-loading diagnostics | `-debug_xasset` (off by default) |
| Scaleform action tracing | `-scaleform-actions` (off by default) |
| Offline player name | Optional `-set name <value>`, only when Steam is not detected |

The name field accepts up to 15 printable ASCII characters, matching the native
15-byte display cache and avoiding the shim's locale-dependent command-line
conversion. Command/userinfo syntax is rejected. Blank preserves the existing
name. It is disabled while Steam runs in the current Windows session, and if
Steam status cannot be read. A three-second refresh and window activation keep
the UI current; the launch boundary checks again after any update delay, and
omits a stored name when locked. This is deliberately conservative: a running
Steam process is not proof of SDK initialization or `BLoggedOn`. The game owns
actual Steam identity selection; the launcher never initializes Steam or changes
`steam_useSteamName` to bypass it.

No shell command interpolation or arbitrary extra arguments are accepted. Open
Steam before playing online; select PLAY ONLINE in-game. Without Steam, use
SYSTEM LINK as the LAN fallback. No forced offline/debug flag is inserted.

Updates invoke the launcher's bundled `Support/Check Consolation Updates.ps1`,
retaining its package verification, confirmation, rollback and
debug-build protection. It runs only on explicit update or Play with the
pre-launch checkbox enabled. The UI waits for completion, disables concurrent
operations and blocks closing while the updater runs. The actual report is
shown, including skipped checks and network warnings; zero exit alone is not
labelled "up to date". Exit 2 blocks further launches/updates in this launcher
session until the user restores the reported backup and reopens it. Game
processes must be closed before launch/update. Update failure does not trigger
an automatic game launch; the existing script's nonfatal warning behavior is
preserved when it returns zero.

### Repair client files and progress

Settings includes **Repair client files**. This downloads (or reuses a
hash-verified cached copy of) the latest published nightly package, validates
its manifest, and reinstalls all approved package files, including matching
files. It does not launch the game afterward. The launcher carries the updater
so repair still works if the game-folder copy is missing or obsolete. Either
`JB_Launcher_s.exe` or the original `jb_mp_s.dll` must identify the selected game
root. It is not a replacement for installing the original game.

Before downloading or installing, a confirmation defaults to **No**:

> This will attempt to repair your client by reinstalling the latest nightly
> release. Nightly builds are highly unstable and may introduce new problems.
> If the game still does not work afterward, try reverting to an older nightly
> build.

The dialog also warns that client files, including debug builds, will be
replaced. Repair explicitly permits replacing a debug client; ordinary updates
still skip debug builds. Existing client files receive rollback copies. The
normal package path restrictions remain in place; profiles/identities are not
repair targets. No extra user files are deleted.

The footer displays per-stage progress: downloaded MB, verified file counts,
backup preparation, installation and rollback. Release lookup and archive
hashing are indeterminate. Percentages refer to the current stage, not a guessed
overall completion estimate. Progress records stream from the updater while
the UI remains responsive; closing and concurrent operations remain blocked.
`-Repair` and `-ProgressOutput` are updater-script switches, not game flags.

### Diagnostics and recovery

Launch-option checkboxes save immediately, including **Skip updates
(-noupdate)**. They restore on the next start even if neither Save nor Play was
used. Startup control initialization never overwrites saved toggles, and toggle
autosave does not submit pending folder/name edits. Folder/name edits retain
their explicit Save buttons. Settings live in
`%LOCALAPPDATA%/ProjectConsolation/launcher.json`.

Before launch, the launcher checks the original multiplayer DLL on disk using
the client's existing `multiplayer` marker at RVA `0x0050502C` and requires an
x86 PE32 image. It does not load the DLL. This is a compatibility check, not
authentication or proof of the installation's language. Repair cannot replace
an unsupported original `jb_mp_s.dll`; install the supported game patch.

The Play page shows the installed client resource version (or explicitly says
it is unavailable), debug status, and the selected nightly name/tag/date during
an update. Steam status reports only what is known: a local Steam process does
not prove sign-in or a working game connection. The launcher does not initialize
Steam on the game's behalf.

Results and errors have selectable details, Copy details, Open logs and a wiki
link. Local reports are saved under `%LOCALAPPDATA%/ProjectConsolation/logs`;
they are never automatically uploaded. Window bounds and maximized state are
remembered separately from unsaved launch-option edits, with offscreen bounds
clamped to the current primary work area.

Cancel update requests cooperative cancellation during lookup, downloading or
verification. Network lookup can take up to its timeout before the request is
observed. A cancelled pre-launch update still proceeds to the normal game
validation and launch. Once installation begins, cancellation and closing are
disabled until it finishes or rolls back; no updater process is killed.

Settings lists complete backups made by this updater. Restoration validates
every original's SHA-256 before changing game files, asks for confirmation and
backs up the current copies, allowing another restoration. Newly added files
are retained, so this is file restoration rather than an exact release rollback.
Profiles are not restore targets. Older backups without `backup.json`, incomplete
backups and corrupt backups require manual inspection; they are never inferred
safe from directory names alone. Backup hashes detect corruption, not malicious
modification by another process with the same file permissions.

This subproject is not yet included in nightly archives. Verification includes
23 static launcher contracts, updater fixture tests in PowerShell 7 and Windows
PowerShell 5.1, and offscreen source-XAML renders. The latest source changes have
been built successfully as Release x86. Compiled launch, updater dialogs, DPI, keyboard navigation and
installation behavior still require a Windows runtime test. Run static tests
with unittest discovery in `src/launcher/tests` and updater fixtures with
`tools/test_full_updates.ps1`; fixtures use no real game files or network.
