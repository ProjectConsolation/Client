# Local XLive storage

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

The directory XUID is generated deterministically from the active `name` dvar,
with command-line `name`/`-set name` as the early-startup fallback. It uses the
GFWL-style `E000xxxxxxxxxxxx` range. Changing the name therefore selects a
different offline profile.

## Runtime requirement

`-offline`, `-local_offline`, and `-local-offline` enable the client-side
offline patches and skip the installed-GFWL check. They do not change Windows'
PE loading order: the project's replacement `xlive.dll` must still be placed
where the launcher and `jb_mp_s.dll` can resolve their ordinal imports. The
original system `xlive.dll` is not used for offline behavior.

The usual launch form is:

```text
_s.exe -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 -offline
```
