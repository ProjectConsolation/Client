param(
    [string]$GameDirectory = $PSScriptRoot,
    [switch]$CheckOnly,
    [switch]$LibraryOnly,
    [switch]$NoUpdate
)

if ($NoUpdate) {
    Write-Host '[updater] skipped due to -noupdate'
    return
}

$ErrorActionPreference = 'Stop'

function Test-ConsolationDebugBuild([string]$Root) {
    $client = Resolve-ConsolationUpdatePath $Root 'd3d9.dll'
    if (!(Test-Path -LiteralPath $client -PathType Leaf)) { return $false }
    # resource.rc sets VS_FF_DEBUG for _DEBUG. Read without loading the DLL.
    return [Diagnostics.FileVersionInfo]::GetVersionInfo($client).IsDebug
}

function Get-ConsolationUpdateDisposition([bool]$DebugBuild, [int]$ChangedFiles) {
    if ($ChangedFiles -eq 0) { return 'current' }
    if ($DebugBuild) { return 'skip-debug' }
    return 'prompt'
}

function Get-ConsolationBlobHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA1]::Create()
    try {
        # Git tree IDs hash the blob header as well as its bytes.
        $header = [Text.Encoding]::ASCII.GetBytes("blob $($stream.Length)`0")
        [void]$sha.TransformBlock($header, 0, $header.Length, $header, 0)
        $buffer = New-Object byte[] 65536
        while (($count = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            [void]$sha.TransformBlock($buffer, 0, $count, $buffer, 0)
        }
        [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        return [BitConverter]::ToString($sha.Hash).Replace('-', '').ToLowerInvariant()
    } finally { $stream.Dispose(); $sha.Dispose() }
}

function Resolve-ConsolationUpdatePath([string]$Root, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or $Relative.Contains('\')) { throw 'Invalid update path' }
    foreach ($part in $Relative.Split('/')) {
        if (!$part -or $part -in @('.', '..') -or $part -match '[<>:"|?*\x00-\x1f]' -or
            $part -match '[. ]$' -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') {
            throw "Unsafe update path: $Relative"
        }
    }
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $target = [IO.Path]::GetFullPath((Join-Path $base $Relative))
    if (!$target.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw 'Update escaped its root' }
    # Do not follow a junction/symlink into another directory.
    $current = $target
    while ($current -and $current.Length -ge $base.TrimEnd('\').Length) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Update path contains a reparse point: $current"
            }
        }
        $current = Split-Path -Parent $current
    }
    return $target
}

function Get-ConsolationUpdatePlan([string]$Root, $Tree) {
    if ($Tree.truncated) { throw 'GitHub returned an incomplete file tree' }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $total = 0L
    foreach ($entry in $Tree.tree) {
        if (!$entry.path.StartsWith('required_files/', [StringComparison]::Ordinal)) { continue }
        if ($entry.type -eq 'tree') { continue }
        if ($entry.type -ne 'blob' -or $entry.mode -notin @('100644', '100755')) { throw 'Unsupported update entry' }
        $relative = $entry.path.Substring(15)
        $name = [IO.Path]::GetFileName($relative)
        if ($name -in @('AGENTS.md', 'AGENTS.override.md', 'xlive.dll', 'steam_api.dll', 'playlists.info') -or
            [IO.Path]::GetExtension($relative) -in @('.lnk', '.sum', '.pdb', '.lib', '.exp')) { continue }
        if (!$seen.Add($relative)) { throw 'Case-colliding update paths' }
        if ($entry.sha -notmatch '^[0-9a-f]{40}$' -or $entry.size -lt 0 -or $entry.size -gt 134217728) { throw 'Invalid update metadata' }
        $total += $entry.size
        if ($total -gt 536870912) { throw 'Required-files update is too large' }
        $target = Resolve-ConsolationUpdatePath $Root $relative
        if (!(Test-Path -LiteralPath $target -PathType Leaf) -or (Get-ConsolationBlobHash $target) -ne $entry.sha) {
            [pscustomobject]@{ Relative = $relative; Target = $target; Sha = $entry.sha; Size = $entry.size }
        }
    }
}

function Save-ConsolationUpdateFile([string]$Commit, $Entry, [string]$Destination) {
    if ($Entry.SourcePath) {
        Copy-Item -LiteralPath $Entry.SourcePath -Destination $Destination
        if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $Entry.Sha256) {
            throw 'Staged package file hash mismatch'
        }
        return
    }
    $encoded = ($Entry.Relative.Split('/') | ForEach-Object { [Uri]::EscapeDataString($_) }) -join '/'
    $url = "https://raw.githubusercontent.com/ProjectConsolation/Client/$Commit/required_files/$encoded"
    Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 30 -OutFile $Destination | Out-Null
    if ((Get-Item -LiteralPath $Destination).Length -ne $Entry.Size -or
        (Get-ConsolationBlobHash $Destination) -ne $Entry.Sha) { throw "Downloaded hash mismatch: $($Entry.Relative)" }
    if ($Entry.Size -lt 1024) {
        if ([IO.File]::ReadAllText($Destination).StartsWith('version https://git-lfs.github.com/spec/v1')) {
            throw 'Git LFS pointer is not an installable file'
        }
    }
}

function Install-ConsolationUpdate([string]$Root, [string]$Commit, [object[]]$Plan) {
    if (($Plan | Where-Object { !$_.SourcePath }) -and $Commit -notmatch '^[0-9a-f]{40}$') { throw 'Invalid pinned revision' }
    $backupRoot = Resolve-ConsolationUpdatePath $Root ('consolation-update-backups/' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backupRoot | Out-Null
    $applied = New-Object 'System.Collections.Generic.List[object]'
    try {
        # Download and verify the ENTIRE update before changing installed files.
        foreach ($entry in $Plan) {
            $staged = Resolve-ConsolationUpdatePath $backupRoot ('downloads/' + $entry.Relative)
            New-Item -ItemType Directory -Path (Split-Path -Parent $staged) -Force | Out-Null
            Save-ConsolationUpdateFile $Commit $entry $staged
        }
        foreach ($entry in $Plan) {
            $target = Resolve-ConsolationUpdatePath $Root $entry.Relative
            $old = Resolve-ConsolationUpdatePath $backupRoot ('originals/' + $entry.Relative)
            $existed = Test-Path -LiteralPath $target -PathType Leaf
            if ($existed) {
                New-Item -ItemType Directory -Path (Split-Path -Parent $old) -Force | Out-Null
                Copy-Item -LiteralPath $target -Destination $old
            }
            $applied.Add([pscustomobject]@{ Target = $target; Old = $old; Existed = $existed })
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath (Resolve-ConsolationUpdatePath $backupRoot ('downloads/' + $entry.Relative)) -Destination $target -Force
            $installedHash = if ($entry.Sha256) { (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash } else { Get-ConsolationBlobHash $target }
            $expectedHash = if ($entry.Sha256) { $entry.Sha256 } else { $entry.Sha }
            if ($installedHash -ne $expectedHash) { throw 'Installed hash mismatch' }
        }
    } catch {
        $failure = $_
        $restoreFailures = @()
        for ($i = $applied.Count - 1; $i -ge 0; --$i) {
            $item = $applied[$i]
            try {
                if ($item.Existed) { Copy-Item -LiteralPath $item.Old -Destination $item.Target -Force }
                elseif (Test-Path -LiteralPath $item.Target -PathType Leaf) { Remove-Item -LiteralPath $item.Target }
            } catch { $restoreFailures += $item.Target }
        }
        if ($restoreFailures.Count) {
            throw "Update failed; manual restoration required for $($restoreFailures -join ', '). Backups: $backupRoot. $failure"
        }
        throw "Update failed; previous files restored. Backup: $backupRoot. $failure"
    }
    return $backupRoot
}

function Get-ConsolationPackagePlan([string]$Root, [string]$ArchivePath, [string]$Staging) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entries = @{}
        foreach ($zipEntry in $archive.Entries) {
            $name = $zipEntry.FullName.Replace('\', '/')
            if ($name.EndsWith('/')) { continue }
            [void](Resolve-ConsolationUpdatePath $Staging $name)
            if ($entries.ContainsKey($name)) { throw 'Duplicate/case-colliding ZIP entry' }
            $entries[$name] = $zipEntry
        }
        $manifestEntry = $entries['consolation-update-manifest.json']
        if (!$manifestEntry -or $manifestEntry.Length -gt 2097152) { throw 'Release lacks a compatible full-update manifest; install a newer complete release manually' }
        $reader = New-Object IO.StreamReader($manifestEntry.Open())
        try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($manifest.schema -ne 1 -or !$manifest.files -or $manifest.files.Count -gt 10000) { throw 'Invalid full-update manifest' }
        $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $total = 0L
        foreach ($file in $manifest.files) {
            $name = [string]$file.path
            $target = Resolve-ConsolationUpdatePath $Root $name
            if (!$seen.Add($name) -or !$entries.ContainsKey($name)) { throw 'Invalid manifest paths' }
            $zipEntry = $entries[$name]
            if ($file.sha256 -notmatch '^[0-9a-f]{64}$' -or $file.size -lt 0 -or
                $file.size -gt 134217728 -or $zipEntry.Length -ne $file.size) { throw 'Invalid manifest hash/size' }
            $total += $file.size
            if ($total -gt 1073741824) { throw 'Unpacked release exceeds limit' }
            # Never overwrite profiles, user configs, identities or system files.
            # Ignore this obsolete entry in older manifests before the allowlist
            # check; it must never be an approved installable required file.
            if ($name -eq 'xlive_patch.asi') { continue }
            $rootFiles = @('d3d9.dll','xlive.dll','steam_api.dll','companion64.dll','binkw32.dll',
                'JB_Launcher_s.exe','Launch Consolation.bat','Check Consolation Updates.ps1',
                'README.txt','README_XLIVE.txt','PLAYTEST_STEAM_PLAYLISTS.txt',
                'Repair Consolation Shortcut.ps1','consolelogo.png','icon.ico','jb.bmp','jblogo.bmp')
            if ($name -notin $rootFiles -and !$name.StartsWith('consolation/') -and
                !$name.StartsWith('licenses/') -and $name -ne 'optional/offline/xlive.dll') { throw "Unexpected package path: $name" }
            # Old release packages may still contain the redundant loose rawfile.
            if ([IO.Path]::GetFileName($name) -eq 'playlists.info') { continue }
            $source = Resolve-ConsolationUpdatePath $Staging $name
            New-Item -ItemType Directory -Path (Split-Path -Parent $source) -Force | Out-Null
            $inputStream = $zipEntry.Open()
            $outputStream = [IO.File]::Create($source)
            try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
            if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) { throw 'Release file hash mismatch' }
        }
        foreach ($required in @('d3d9.dll','steam_api.dll','Launch Consolation.bat','Check Consolation Updates.ps1',
                'consolation/zone/common_consolation.ff','consolation/zone/patch_consolation.ff')) {
            if (!$seen.Contains($required)) { throw "Incomplete full release: $required" }
        }
        # New releases install the required shim directly; accept the older
        # optional layout as well so cached releases remain compatible.
        if (!$seen.Contains('xlive.dll') -and !$seen.Contains('optional/offline/xlive.dll')) {
            throw 'Incomplete full release: xlive.dll'
        }
        if ($entries.Count -ne $seen.Count + 1) { throw 'Unmanifested ZIP files' }
        foreach ($file in $manifest.files) {
            $name = [string]$file.path
            if ([IO.Path]::GetFileName($name) -eq 'playlists.info') { continue }
            $target = Resolve-ConsolationUpdatePath $Root $name
            $source = Resolve-ConsolationUpdatePath $Staging $name
            if (!(Test-Path -LiteralPath $target -PathType Leaf) -or
                (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.sha256) {
                [pscustomobject]@{ Relative=$name; Target=$target; SourcePath=$source; Sha256=$file.sha256; Size=$file.size }
            }
            # Only update a root offline DLL when it is the installed copy of
            # the old bundled shim. Never replace retail/system GFWL implicitly.
            if ($name -eq 'optional/offline/xlive.dll' -and !$seen.Contains('xlive.dll')) {
                $rootShim = Resolve-ConsolationUpdatePath $Root 'xlive.dll'
                if ((Test-Path -LiteralPath $rootShim -PathType Leaf) -and (Test-Path -LiteralPath $target -PathType Leaf)) {
                    $rootHash = (Get-FileHash -LiteralPath $rootShim -Algorithm SHA256).Hash
                    if ($rootHash -eq (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -and $rootHash -ne $file.sha256 -and
                        [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($rootShim)).Contains('ConsolationXLiveShim')) {
                        [pscustomobject]@{ Relative='xlive.dll'; Target=$rootShim; SourcePath=$source; Sha256=$file.sha256; Size=$file.size }
                    }
                }
            }
        }
    } finally { $archive.Dispose() }
}

if ($LibraryOnly) { return }
try {
    $GameDirectory = [IO.Path]::GetFullPath($GameDirectory)
    if ($GameDirectory.TrimEnd('\') -eq [IO.Path]::GetPathRoot($GameDirectory).TrimEnd('\') -or
        !(Test-Path -LiteralPath (Join-Path $GameDirectory 'JB_Launcher_s.exe') -PathType Leaf)) { throw 'Not a game installation directory' }
    if (Get-Process -Name JB_Launcher_s, JB_LiveEngine_s -ErrorAction SilentlyContinue) { throw 'Close the game before checking for updates' }
    $debugBuild = Test-ConsolationDebugBuild $GameDirectory
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $headers = @{ 'User-Agent' = 'ProjectConsolation-Full-Updater'; 'Accept' = 'application/vnd.github+json' }
    $releases = Invoke-RestMethod -Uri 'https://api.github.com/repos/ProjectConsolation/Client/releases?per_page=20' -Headers $headers -TimeoutSec 10
    $release = $releases | Where-Object { !$_.draft -and ($_.assets.name -contains 'consolation-nightly.zip') } |
        Sort-Object published_at -Descending | Select-Object -First 1
    if (!$release) { throw 'No published client package found' }
    $asset = @($release.assets | Where-Object { $_.name -eq 'consolation-nightly.zip' })
    if ($asset.Count -ne 1 -or $asset[0].digest -notmatch '^sha256:[0-9a-f]{64}$' -or
        $asset[0].size -le 0 -or $asset[0].size -gt 536870912 -or
        !$asset[0].browser_download_url.StartsWith('https://github.com/ProjectConsolation/Client/releases/download/')) { throw 'Release lacks a verified download digest' }
    $asset = $asset[0]
    $digest = $asset.digest.Substring(7)
    $cache = Join-Path $env:LOCALAPPDATA 'ProjectConsolation/update-cache'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $package = Join-Path $cache ($digest + '.zip')
    if (!(Test-Path -LiteralPath $package -PathType Leaf) -or (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $digest) {
        Write-Host 'Downloading the complete client update; please wait...'
        Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -TimeoutSec 120 -OutFile $package | Out-Null
    }
    if ((Get-Item -LiteralPath $package).Length -ne $asset.size -or (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $digest) { throw 'Release archive hash mismatch' }
    $staging = Join-Path $cache ([Guid]::NewGuid().ToString('N'))
    $plan = @(Get-ConsolationPackagePlan $GameDirectory $package $staging)
    $disposition = Get-ConsolationUpdateDisposition $debugBuild $plan.Count
    if ($disposition -eq 'current') { Write-Host 'Consolation is up to date.'; return }
    if ($disposition -eq 'skip-debug') {
        Write-Host '[updater] update available, but skipped due to debug build'
        return
    }
    $fastfiles = @($plan | Where-Object { [IO.Path]::GetExtension($_.Relative) -eq '.ff' }).Count
    $info = $release.name + ' [' + $release.tag_name + "]`n$($plan.Count) file(s) differ, including $fastfiles new/changed fastfile(s)."
    if ($CheckOnly) { Write-Output $info; return }
    Add-Type -AssemblyName System.Windows.Forms
    $message = "An update is available ($info). Do you want to update? Some updates may break game functionality if you are running an older build.`n`nThis installs matching DLLs, fastfiles and support files from one complete release. No starts the game and asks again next time."
    $answer = [Windows.Forms.MessageBox]::Show($message, 'Project: Consolation update', [Windows.Forms.MessageBoxButtons]::YesNo,
        [Windows.Forms.MessageBoxIcon]::Warning, [Windows.Forms.MessageBoxDefaultButton]::Button2)
    if ($answer -ne [Windows.Forms.DialogResult]::Yes) { return }
    if (Test-ConsolationDebugBuild $GameDirectory) {
        Write-Host '[updater] update available, but skipped due to debug build'
        return
    }
    if (Get-Process -Name JB_Launcher_s, JB_LiveEngine_s -ErrorAction SilentlyContinue) { throw 'Game started during the update check; installation deferred until next launch' }
    $backup = Install-ConsolationUpdate $GameDirectory $release.tag_name $plan
    Write-Host "Full client update complete. Rollback copies: $backup"
} catch {
    if ($_.Exception.Message -match 'manual restoration required') {
        Write-Warning $_
        exit 2 # Do not launch a possibly mixed installation after failed rollback.
    }
    Write-Warning "Consolation update check/update failed: $_. Continuing startup."
}
