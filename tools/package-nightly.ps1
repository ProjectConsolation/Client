param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReleaseDirectory,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if (!$ReleaseDirectory) { $ReleaseDirectory = Join-Path $RepositoryRoot 'build/bin/Release' }
if (!$OutputPath) { $OutputPath = Join-Path $RepositoryRoot 'consolation-nightly.zip' }
$requiredDirectory = Join-Path $RepositoryRoot 'required_files'
$companionFiles = @()
$companionPath = Join-Path $requiredDirectory 'companion64.dll'
if (Test-Path -LiteralPath $companionPath -PathType Leaf) {
    $companionBytes = [System.IO.File]::ReadAllBytes($companionPath)
    if ($companionBytes.Length -lt 64 -or [System.BitConverter]::ToUInt16($companionBytes, 0) -ne 0x5A4D) {
        throw 'companion64.dll is not a Windows PE image'
    }
    $companionPeOffset = [System.BitConverter]::ToInt32($companionBytes, 0x3C)
    if ($companionPeOffset -lt 64 -or $companionPeOffset -gt ($companionBytes.Length - 26) -or
            [System.BitConverter]::ToUInt32($companionBytes, $companionPeOffset) -ne 0x4550 -or
            [System.BitConverter]::ToUInt16($companionBytes, $companionPeOffset + 4) -ne 0x8664 -or
            [System.BitConverter]::ToUInt16($companionBytes, $companionPeOffset + 24) -ne 0x20B) {
        throw 'Companion requires the native Windows AMD64 DLL, not an x86/Linux module'
    }
    $companionFiles = @('companion64.dll', 'licenses/companion/LICENSE.md',
        'licenses/companion/LICENSE-EXCEPTION.md', 'licenses/companion/LEGAL',
        'licenses/companion/AUTHORS', 'licenses/companion/SOURCE.txt',
        'licenses/companion/companion-source.zip', 'licenses/companion/windows-build-fixes.patch')
    foreach ($notice in $companionFiles) {
        if (!(Test-Path -LiteralPath (Join-Path $requiredDirectory $notice) -PathType Leaf)) {
            throw "Companion distribution is missing $notice (SOURCE.txt must identify the matching corresponding source)"
        }
    }
} else {
    Write-Warning 'Companion module is not supplied: custom Steam activity will be unavailable; SDK matchmaking still works.'
}
$stockMenuFiles = @(
    'consolation/menu/ui_mp/steam_lobby.menu',
    'consolation/menu/ui_mp/steam_friends.menu',
    'consolation/scaleform/MpMainMenu.gfx',
    'consolation/scaleform/CmOptions.gfx',
    'consolation/scaleform/cmoptionsgraphics.gfx',
    'consolation/scaleform/cmaftervidrestart.gfx',
    'consolation/scaleform/pcsharedlibrary.gfx'
    'consolation/scaleform/cmsharedplatform.gfx'
    'consolation/scaleform/cmfrontend.gfx'
    'consolation/scaleform/CmOptCntlr.gfx'
    'consolation/scaleform/CmOptCntlrBtns.gfx'
    'consolation/scaleform/CmOptCntlrStks.gfx'
    'consolation/scaleform/cslcontroller.gfx'
    'consolation/scaleform/cslcontrolleradvanced.gfx'
    'consolation/scaleform/mpcustomloadout.gfx'
)

# Package only selected build outputs, never a reused build directory wholesale.
foreach ($name in @('d3d9.dll', 'xlive.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $ReleaseDirectory $name) -PathType Leaf)) {
        throw "Missing build output: $name"
    }
}
# Refuse a 64-bit redistributable in this x86 client package.
$steamApiPath = Join-Path $ReleaseDirectory 'steam_api.dll'
if (!(Test-Path -LiteralPath $steamApiPath -PathType Leaf)) {
    $steamApiPath = Join-Path $requiredDirectory 'steam_api.dll'
}
if (!(Test-Path -LiteralPath $steamApiPath -PathType Leaf)) {
    throw 'Missing Steam runtime: steam_api.dll must be in build output or required_files'
}
$steamBytes = [System.IO.File]::ReadAllBytes($steamApiPath)
if ($steamBytes.Length -lt 64 -or [System.BitConverter]::ToUInt16($steamBytes, 0) -ne 0x5A4D) {
    throw 'steam_api.dll is not a Windows PE image'
}
$steamPeOffset = [System.BitConverter]::ToInt32($steamBytes, 0x3C)
if ($steamPeOffset -lt 64 -or $steamPeOffset -gt ($steamBytes.Length - 6) -or
        [System.BitConverter]::ToUInt32($steamBytes, $steamPeOffset) -ne 0x4550 -or
        [System.BitConverter]::ToUInt16($steamBytes, $steamPeOffset + 4) -ne 0x14C) {
    throw 'Nightly requires the Windows x86 steam_api.dll, not steam_api64.dll'
}
foreach ($name in @('Launch Consolation.bat', 'Check Consolation Updates.ps1', 'JB_Launcher_s.exe', 'README.txt',
        'README_XLIVE.txt', 'consolation/zone/common_consolation.ff', 'consolation/zone/patch_consolation.ff') + $stockMenuFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $requiredDirectory $name) -PathType Leaf)) {
        throw "Missing required release file: $name"
    }
}

$stagingParent = Join-Path $RepositoryRoot 'build/nightly-packages'
$staging = Join-Path $stagingParent ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging -Force | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $requiredDirectory -Recurse -File) {
    if ($file.Name -in @('AGENTS.md', 'AGENTS.override.md', 'xlive.dll', 'steam_api.dll', 'playlists.info', 'xlive_patch.asi') -or
            $file.Extension -in @('.lnk', '.sum', '.pdb', '.lib', '.exp')) { continue }
    $relative = [System.IO.Path]::GetRelativePath($requiredDirectory, $file.FullName)
    $destination = Join-Path $staging $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $ReleaseDirectory 'd3d9.dll') -Destination $staging
Copy-Item -LiteralPath $steamApiPath -Destination $staging
$offline = Join-Path $staging 'optional/offline'
New-Item -ItemType Directory -Path $offline -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $ReleaseDirectory 'xlive.dll') -Destination $offline

# A full updater must install matching code and assets from one release, not
# combine main-branch assets with binaries from an older nightly.
$updateFiles = @(Get-ChildItem -LiteralPath $staging -File -Recurse | ForEach-Object {
    [pscustomobject]@{
        path = [System.IO.Path]::GetRelativePath($staging, $_.FullName).Replace('\', '/')
        size = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
[pscustomobject]@{ schema = 1; files = $updateFiles } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $staging 'consolation-update-manifest.json') -Encoding UTF8

Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $OutputPath -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead([System.IO.Path]::GetFullPath($OutputPath))
try {
    $names = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    if ('consolation-update-manifest.json' -notin $names) { throw 'Full-update manifest missing from ZIP' }
    foreach ($required in @('d3d9.dll', 'steam_api.dll', 'Launch Consolation.bat', 'Check Consolation Updates.ps1', 'JB_Launcher_s.exe',
            'README.txt', 'README_XLIVE.txt', 'consolation/zone/common_consolation.ff', 'consolation/zone/patch_consolation.ff',
            'optional/offline/xlive.dll') + $stockMenuFiles + $companionFiles) {
        if ($required -notin $names) { throw "Nightly ZIP is missing $required" }
    }
    if ('xlive.dll' -in $names) { throw 'Offline replacement must not be installed from the ZIP root' }
    if ('xlive_patch.asi' -in $names) { throw 'Legacy ASI is incompatible with the bundled offline shim' }
    if ($names | Where-Object { $_ -match '(^|/)playlists\.info$' }) { throw 'Playlists must be embedded in patch_consolation.ff, not loose files' }
    # Verify the packaged menu bytes, not just their filenames. The shared PC
    # library must travel with the screens whose class definitions it supplies.
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($menu in $stockMenuFiles + $companionFiles + @('consolation/zone/common_consolation.ff', 'consolation/zone/patch_consolation.ff', 'steam_api.dll')) {
            $entry = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $menu }
            $stream = $entry.Open()
            try { $packedHash = [System.BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '') }
            finally { $stream.Dispose() }
            $hashSource = if ($menu -eq 'steam_api.dll') { $steamApiPath } else { Join-Path $requiredDirectory $menu }
            $sourceHash = (Get-FileHash -LiteralPath $hashSource -Algorithm SHA256).Hash
            if ($packedHash -ne $sourceHash) { throw "Nightly menu hash mismatch: $menu" }
        }
    } finally { $sha256.Dispose() }
    if ($names | Where-Object { $_ -match '(^|/)(AGENTS(\.override)?\.md|.*\.sum|.*\.lnk)$' }) {
        throw 'Nightly ZIP contains excluded development/legacy files'
    }
} finally { $archive.Dispose() }
Write-Output "Verified nightly package: $OutputPath"
