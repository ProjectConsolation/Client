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
$stockMenuFiles = @(
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
foreach ($name in @('Launch Consolation.bat', 'JB_Launcher_s.exe', 'README.txt',
        'README_XLIVE.txt', 'consolation/zone/common_consolation.ff', 'consolation/zone/patch_consolation.ff') + $stockMenuFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $requiredDirectory $name) -PathType Leaf)) {
        throw "Missing required release file: $name"
    }
}

$stagingParent = Join-Path $RepositoryRoot 'build/nightly-packages'
$staging = Join-Path $stagingParent ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging -Force | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $requiredDirectory -Recurse -File) {
    if ($file.Name -in @('AGENTS.md', 'AGENTS.override.md', 'xlive.dll') -or
            $file.Extension -in @('.lnk', '.sum', '.pdb', '.lib', '.exp')) { continue }
    $relative = [System.IO.Path]::GetRelativePath($requiredDirectory, $file.FullName)
    $destination = Join-Path $staging $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $ReleaseDirectory 'd3d9.dll') -Destination $staging
$offline = Join-Path $staging 'optional/offline'
New-Item -ItemType Directory -Path $offline -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $ReleaseDirectory 'xlive.dll') -Destination $offline

Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $OutputPath -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead([System.IO.Path]::GetFullPath($OutputPath))
try {
    $names = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($required in @('d3d9.dll', 'Launch Consolation.bat', 'JB_Launcher_s.exe',
            'README.txt', 'README_XLIVE.txt', 'consolation/zone/common_consolation.ff', 'consolation/zone/patch_consolation.ff',
            'optional/offline/xlive.dll') + $stockMenuFiles) {
        if ($required -notin $names) { throw "Nightly ZIP is missing $required" }
    }
    if ('xlive.dll' -in $names) { throw 'Offline replacement must not be installed from the ZIP root' }
    # Verify the packaged menu bytes, not just their filenames. The shared PC
    # library must travel with the screens whose class definitions it supplies.
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($menu in $stockMenuFiles + @('consolation/zone/common_consolation.ff', 'consolation/zone/patch_consolation.ff')) {
            $entry = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $menu }
            $stream = $entry.Open()
            try { $packedHash = [System.BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '') }
            finally { $stream.Dispose() }
            $sourceHash = (Get-FileHash -LiteralPath (Join-Path $requiredDirectory $menu) -Algorithm SHA256).Hash
            if ($packedHash -ne $sourceHash) { throw "Nightly menu hash mismatch: $menu" }
        }
    } finally { $sha256.Dispose() }
    if ($names | Where-Object { $_ -match '(^|/)(AGENTS(\.override)?\.md|.*\.sum|.*\.lnk)$' }) {
        throw 'Nightly ZIP contains excluded development/legacy files'
    }
} finally { $archive.Dispose() }
Write-Output "Verified nightly package: $OutputPath"
