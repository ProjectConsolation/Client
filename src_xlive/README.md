# Local XLive storage

The replacement `xlive.dll` is optional and intended only for offline/debug
use, not normal online GFWL operation. Building `d3d9` also builds the shim,
but does not install it automatically. The output includes
`README_XLIVE.txt`; copy the DLL into the game root explicitly when needed.

The offline shim stores emulated Games for Windows - LIVE state beside the
replacement `xlive.dll`. When the DLL is deployed in the game root, this is
`root\storage`:

```text
storage/
  profiles/
    <16-digit XUID>/
      settings/
        41560829/
          63E83FFD.bin
          63E83FFE.bin
          63E83FFF.bin
      title_storage/
        <facility>/
          <item name>
```

`41560829` is the Quantum of Solace PC title ID. The three `63E83FFD` through
`63E83FFF` files are the raw payloads of the SDK's title-specific binary profile
settings. Files under `title_storage` are the payloads handled by
`XStorageUploadFromMemory` and `XStorageDownloadToMemory`, such as `mpdata`.

On the first profile-settings read, the shim searches
`%LOCALAPPDATA%\Microsoft\Xlive\Content` for `41560829.gpd`. It validates the
little-endian `FBDX`/XDBF tables and imports namespace 3 binary-setting payloads
that do not already exist locally. Account data, signatures, achievements, and
other games are deliberately not copied.

Normal offline network XUIDs are randomly generated once and stored in
`%LOCALAPPDATA%\ConsolationClient\offline-identities\<instance>.bin`.
They use the GFWL-style `E000xxxxxxxxxxxx` range and do not depend on the
player's name, Discord account, or hardware identifiers. Renaming the player
does not change the network identity. Separate Windows accounts/installations
generate independent identities; copying these identity files also copies the
identity, so do not distribute them with builds. This is local identity, not
proof of ownership or developer verification.

On first use, the identity record retains the previous name-derived storage
directory key so existing settings and title storage stay in place. That
directory key remains fixed after a rename. Back up both the identity record
and `storage` when transferring a profile. Corrupt identity records are refused
rather than silently regenerated or overwritten. `-offline_test` retains its
isolated process-unique identities and storage.

## Runtime requirement

The replacement shim uses experimental plaintext LAN transport with versioned
identity envelopes for native gameplay and System Link discovery.
`-offline_test` enables process-unique test XUIDs and separate
`storage/debug-net/<machine-pid-start-time>/` persistence.
Legacy GFWL imports are skipped in this mode. `-offline_test_port=<port>` reserves
the gameplay port and adjacent discovery port before native socket binding.
Use base ports 1000 and 1002 for two playable instances on one machine.
Both peers require matching client/shim versions. No encryption,
peer authentication or NAT traversal is provided. Native engine configs are
not isolated; client console logs get per-process test filenames. See `docs/wiki/Commands.md` for
the runtime proof checklist and dedicated-host limitations.

`-offline`, `-local_offline`, and `-local-offline` enable the client-side
offline patches and skip the installed-GFWL check. They do not change Windows'
PE loading order: the project's replacement `xlive.dll` must still be placed
where the launcher and `jb_mp_s.dll` can resolve their ordinal imports. The
original system `xlive.dll` is not used for offline behavior.

The usual launch form is:

```text
_s.exe -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 -offline
```
