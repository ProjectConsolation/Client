param(
    [string]$ProcessName = "JB_LiveEngine_s",
    [int]$MaterialLimit = 12,
    [int]$VisibilitySamples = 1
)

$nativeSource = @"
using System;
using System.Runtime.InteropServices;

public static class ProcessMemory
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(
        IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr read);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
"@

Add-Type -TypeDefinition $nativeSource

$process = @(Get-Process -Name $ProcessName -ErrorAction Stop)[0]
$module = @($process.Modules | Where-Object ModuleName -eq "jb_mp_s.dll")[0]
if (-not $module) {
    throw "jb_mp_s.dll is not loaded in process $($process.Id)"
}

$handle = [ProcessMemory]::OpenProcess(0x410, $false, $process.Id)
if ($handle -eq [IntPtr]::Zero) {
    throw "OpenProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

function Read-Bytes([uint32]$Address, [int]$Count) {
    $buffer = [byte[]]::new($Count)
    $read = [IntPtr]::Zero
    if (-not [ProcessMemory]::ReadProcessMemory(
            $handle, [IntPtr]::new([int64]$Address), $buffer, $Count, [ref]$read) -or
        $read.ToInt64() -ne $Count) {
        throw "ReadProcessMemory failed at 0x$($Address.ToString('X8'))"
    }
    return $buffer
}

function Read-U32([uint32]$Address) {
    return [BitConverter]::ToUInt32((Read-Bytes $Address 4), 0)
}

function Write-AabbTree([uint32]$Address, [int]$Depth = 0, [int]$MaxDepth = 3) {
    if ($Address -eq 0 -or $Depth -gt $MaxDepth) {
        return
    }

    $record = Read-Bytes $Address 48
    $values = 0..5 | ForEach-Object { [BitConverter]::ToSingle($record, $_ * 4) }
    $surfaceCount = [BitConverter]::ToUInt32($record, 24)
    $firstSurface = [BitConverter]::ToUInt32($record, 28)
    $indexCount = [BitConverter]::ToUInt32($record, 32)
    $indexes = [BitConverter]::ToUInt32($record, 36)
    $childCount = [BitConverter]::ToUInt32($record, 40)
    $children = [BitConverter]::ToUInt32($record, 44)
    Write-Output ("aabb depth={0} address=0x{1:X8} midpoint=({2},{3},{4}) half=({5},{6},{7}) surfaces={8}@{9} indexes={10}@0x{11:X8} children={12}@0x{13:X8}" -f
        $Depth, $Address, $values[0], $values[1], $values[2],
        $values[3], $values[4], $values[5], $surfaceCount, $firstSurface,
        $indexCount, $indexes, $childCount, $children)

    if ($Depth -lt $MaxDepth -and $children -ne 0) {
        for ($index = 0; $index -lt [Math]::Min($childCount, 32); ++$index) {
            Write-AabbTree ($children + $index * 48) ($Depth + 1) $MaxDepth
        }
    }
}

function Read-CString([uint32]$Address, [int]$Limit = 256) {
    if ($Address -eq 0) {
        return "<null>"
    }
    $bytes = Read-Bytes $Address $Limit
    $length = [Array]::IndexOf($bytes, [byte]0)
    if ($length -lt 0) {
        $length = $Limit
    }
    return [Text.Encoding]::ASCII.GetString($bytes, 0, $length)
}

try {
    $moduleBase = [uint32]$module.BaseAddress.ToInt64()
    $worldPointerAddress = $moduleBase + 0x00C4A354
    $world = Read-U32 $worldPointerAddress
    $cgInitialized = Read-U32 ($moduleBase + 0x029FE8E4)
    if ($cgInitialized -ne 0) {
        $motion = Read-Bytes ($moduleBase + 0x02A4CE1C) 24
        $origin = 0..2 | ForEach-Object { [BitConverter]::ToSingle($motion, $_ * 4) }
        $velocity = 0..2 | ForEach-Object { [BitConverter]::ToSingle($motion, 12 + $_ * 4) }
        Write-Output ("player origin=({0},{1},{2}) velocity=({3},{4},{5})" -f
            $origin[0], $origin[1], $origin[2],
            $velocity[0], $velocity[1], $velocity[2])
    }
    $surfaceCount = Read-U32 ($world + 32)
    $surfaces = Read-U32 ($world + 36)
    $vertexCount = Read-U32 ($world + 80)
    $vertices = Read-U32 ($world + 84)
    Write-Output ("pid={0} module=0x{1:X8} world=0x{2:X8} surfaces={3} array=0x{4:X8} vertices={5} vertexArray=0x{6:X8}" -f
        $process.Id, $moduleBase, $world, $surfaceCount, $surfaces,
        $vertexCount, $vertices)

    $reflectionProbeCount = Read-U32 ($world + 264)
    $reflectionProbes = Read-U32 ($world + 268)
    $lightmapCount = Read-U32 ($world + 300)
    $lightmaps = Read-U32 ($world + 304)
    Write-Output ("reflectionProbes={0} array=0x{1:X8} lightmaps={2} array=0x{3:X8}" -f
        $reflectionProbeCount, $reflectionProbes, $lightmapCount, $lightmaps)

    $cellCount = Read-U32 ($world + 288)
    $cells = Read-U32 ($world + 296)
    $brushModelCount = Read-U32 ($world + 352)
    $brushModels = Read-U32 ($world + 356)
    $dpvsWorldCount = Read-U32 ($world + 360)
    $dpvsWorlds = Read-U32 ($world + 364)
    $dpvsPlaneHeader = Read-Bytes ($world + 308) 52
    $dpvsPlaneWords = for ($word = 0; $word -lt 13; ++$word) {
        "{0:X8}" -f [BitConverter]::ToUInt32($dpvsPlaneHeader, $word * 4)
    }
    Write-Output ("dpvsPlanes words={0}" -f ($dpvsPlaneWords -join " "))
    Write-Output ("cells={0} array=0x{1:X8} brushModels={2} array=0x{3:X8} dpvsWorlds={4} array=0x{5:X8}" -f
        $cellCount, $cells, $brushModelCount, $brushModels,
        $dpvsWorldCount, $dpvsWorlds)
    if ($cellCount -gt 0 -and $cells -ne 0) {
        $cellTree = Read-U32 ($cells + 24)
        $cellWords = for ($word = 0; $word -lt 13; ++$word) {
            "{0:X8}" -f (Read-U32 ($cells + $word * 4))
        }
        Write-Output ("cell0 words={0} tree=0x{1:X8}" -f
            ($cellWords -join " "), $cellTree)
        if ($cellTree -ne 0) {
            $treeWords = for ($word = 0; $word -lt 12; ++$word) {
                "{0:X8}" -f (Read-U32 ($cellTree + $word * 4))
            }
            Write-Output ("cell0Tree words={0}" -f ($treeWords -join " "))
            Write-AabbTree $cellTree
        }
    }

    $runtimeOffsets = @(584, 588, 592, 596, 600, 604, 608, 612,
                        616, 620, 624, 628, 664, 668, 680, 684, 696, 700, 712)
    $runtimeValues = foreach ($offset in $runtimeOffsets) {
        "+0x{0:X3}=0x{1:X8}" -f $offset, (Read-U32 ($world + $offset))
    }
    Write-Output ("runtime: " + ($runtimeValues -join " "))

    $surfaceRemap = Read-U32 ($world + 696)
    if ($surfaceRemap -ne 0 -and $dpvsWorldCount -gt 0) {
        $remapCount = Read-U32 ($dpvsWorlds + 48)
        $sampleCount = [Math]::Min(16, $remapCount)
        $remapBytes = Read-Bytes $surfaceRemap ($sampleCount * 2)
        $remapValues = for ($index = 0; $index -lt $sampleCount; ++$index) {
            [BitConverter]::ToUInt16($remapBytes, $index * 2)
        }
        Write-Output ("surfaceRemap=0x{0:X8} count={1} first={2}" -f
            $surfaceRemap, $remapCount, ($remapValues -join ","))
    }

    for ($dpvsIndex = 0; $dpvsIndex -lt [Math]::Min(4, $dpvsWorldCount); ++$dpvsIndex) {
        $record = Read-Bytes ($dpvsWorlds + $dpvsIndex * 60) 60
        $words = for ($word = 0; $word -lt 15; ++$word) {
            "{0:X8}" -f [BitConverter]::ToUInt32($record, $word * 4)
        }
        Write-Output ("dpvsWorld={0} words={1}" -f $dpvsIndex, ($words -join " "))
    }

    if ($dpvsWorldCount -gt 0) {
        $primarySurfaceCount = Read-U32 ($dpvsWorlds + 48)
        foreach ($offset in @(600, 604, 608, 612)) {
            $address = Read-U32 ($world + $offset)
            $bytes = $null
            $nonzero = 0
            for ($sample = 0; $sample -lt [Math]::Max(1, $VisibilitySamples); ++$sample) {
                $candidate = Read-Bytes $address $primarySurfaceCount
                $candidateNonzero = ($candidate | Where-Object { $_ -ne 0 }).Count
                if ($candidateNonzero -ge $nonzero) {
                    $bytes = $candidate
                    $nonzero = $candidateNonzero
                }
                if ($VisibilitySamples -gt 1) {
                    Start-Sleep -Milliseconds 5
                }
            }
            Write-Output ("visibility +0x{0:X3}=0x{1:X8} bytes={2} nonzero={3} first={4}" -f
                $offset, $address, $primarySurfaceCount, $nonzero,
                ([BitConverter]::ToString($bytes, 0, [Math]::Min(32, $bytes.Length))))
        }

        $drawTable = Read-U32 ($world + 616)
        $drawBytes = Read-Bytes $drawTable ([Math]::Min($surfaceCount, 64) * 8)
        $drawNonzero = ($drawBytes | Where-Object { $_ -ne 0 }).Count
        Write-Output ("drawTable=0x{0:X8} sampleBytes={1} nonzero={2} first={3}" -f
            $drawTable, $drawBytes.Length, $drawNonzero,
            ([BitConverter]::ToString($drawBytes, 0, [Math]::Min(64, $drawBytes.Length))))
    }

    for ($surfaceIndex = 0; $surfaceIndex -lt [Math]::Min(8, $surfaceCount); ++$surfaceIndex) {
        $surface = Read-Bytes ($surfaces + $surfaceIndex * 48) 48
        $words = for ($word = 0; $word -lt 12; ++$word) {
            "{0:X8}" -f [BitConverter]::ToUInt32($surface, $word * 4)
        }
        Write-Output ("surfaceRaw={0} words={1}" -f $surfaceIndex, ($words -join " "))
    }

    for ($lightmapIndex = 0; $lightmapIndex -lt [Math]::Min(4, $lightmapCount); ++$lightmapIndex) {
        $primary = Read-U32 ($lightmaps + $lightmapIndex * 8)
        $secondary = Read-U32 ($lightmaps + $lightmapIndex * 8 + 4)
        $primaryName = if ($primary) { Read-CString (Read-U32 ($primary + 32)) } else { "<null>" }
        $secondaryName = if ($secondary) { Read-CString (Read-U32 ($secondary + 32)) } else { "<null>" }
        $primaryResource = if ($primary) { Read-U32 ($primary + 4) } else { 0 }
        $secondaryResource = if ($secondary) { Read-U32 ($secondary + 4) } else { 0 }
        $primaryBytes = if ($primary) { Read-Bytes $primary 36 } else { [byte[]]::new(36) }
        $secondaryBytes = if ($secondary) { Read-Bytes $secondary 36 } else { [byte[]]::new(36) }
        Write-Output ("lightmap={0} primary=0x{1:X8}:'{2}' resource=0x{3:X8} semantic={4} category={5} size={6}x{7} secondary=0x{8:X8}:'{9}' resource=0x{10:X8} semantic={11} category={12} size={13}x{14}" -f
            $lightmapIndex, $primary, $primaryName, $primaryResource,
            $primaryBytes[11], $primaryBytes[30],
            [BitConverter]::ToUInt16($primaryBytes, 24),
            [BitConverter]::ToUInt16($primaryBytes, 26),
            $secondary, $secondaryName, $secondaryResource,
            $secondaryBytes[11], $secondaryBytes[30],
            [BitConverter]::ToUInt16($secondaryBytes, 24),
            [BitConverter]::ToUInt16($secondaryBytes, 26))
    }

    for ($vertexIndex = 0; $vertexIndex -lt [Math]::Min(4, $vertexCount); ++$vertexIndex) {
        $vertex = Read-Bytes ($vertices + $vertexIndex * 44) 44
        Write-Output ("vertex={0} xyz=({1},{2},{3}) uv=({4},{5}) lmap=({6},{7}) color=0x{8:X8} normal=0x{9:X8} tangent=0x{10:X8}" -f
            $vertexIndex,
            [BitConverter]::ToSingle($vertex, 0), [BitConverter]::ToSingle($vertex, 4),
            [BitConverter]::ToSingle($vertex, 8), [BitConverter]::ToSingle($vertex, 20),
            [BitConverter]::ToSingle($vertex, 24), [BitConverter]::ToSingle($vertex, 28),
            [BitConverter]::ToSingle($vertex, 32), [BitConverter]::ToUInt32($vertex, 16),
            [BitConverter]::ToUInt32($vertex, 36), [BitConverter]::ToUInt32($vertex, 40))
    }

    $seen = @{}
    for ($surfaceIndex = 0; $surfaceIndex -lt $surfaceCount; ++$surfaceIndex) {
        $material = Read-U32 ($surfaces + $surfaceIndex * 48 + 16)
        if ($material -eq 0 -or $seen.ContainsKey($material)) {
            continue
        }
        $seen[$material] = $true

        $materialBytes = Read-Bytes $material 104
        $materialName = Read-CString ([BitConverter]::ToUInt32($materialBytes, 0))
        $textureCount = $materialBytes[67]
        $techniqueSet = [BitConverter]::ToUInt32($materialBytes, 84)
        $techniqueName = Read-CString (Read-U32 $techniqueSet)
        $textureDefs = [BitConverter]::ToUInt32($materialBytes, 88)
        $stateCount = $materialBytes[69]
        $stateBits = [BitConverter]::ToUInt32($materialBytes, 96)
        $stateSlots = [BitConverter]::ToString($materialBytes, 32, 35).Replace("-", "")
        Write-Output ("surface={0} material=0x{1:X8} name='{2}' textures={3} techset=0x{4:X8}:'{5}' defs=0x{6:X8} stateBits=0x{7:X8} count={8} states={9}" -f
            $surfaceIndex, $material, $materialName, $textureCount, $techniqueSet,
            $techniqueName, $textureDefs, $stateBits, $stateCount, $stateSlots)

        for ($stateIndex = 0; $stateIndex -lt $stateCount; ++$stateIndex) {
            $state = Read-Bytes ($stateBits + $stateIndex * 8) 8
            Write-Output ("  state={0} loadBits=0x{1:X8} blendBits=0x{2:X8}" -f
                $stateIndex, [BitConverter]::ToUInt32($state, 0),
                [BitConverter]::ToUInt32($state, 4))
        }

        for ($textureIndex = 0; $textureIndex -lt $textureCount; ++$textureIndex) {
            $definition = Read-Bytes ($textureDefs + $textureIndex * 12) 12
            $textureHash = [BitConverter]::ToUInt32($definition, 0)
            $image = [BitConverter]::ToUInt32($definition, 8)
            if ($image -eq 0 -or $image -eq 0xFFFFFFFF -or $image -eq 0xFFFFFFFE) {
                Write-Output ("  texture={0} hash=0x{1:X8} chars={2:X2}/{3:X2} sampler={4} semantic={5} image=0x{6:X8}" -f
                    $textureIndex, $textureHash, $definition[4], $definition[5],
                    $definition[6], $definition[7], $image)
                continue
            }

            $imageBytes = Read-Bytes $image 36
            $imageName = Read-CString ([BitConverter]::ToUInt32($imageBytes, 32))
            $width = [BitConverter]::ToUInt16($imageBytes, 24)
            $height = [BitConverter]::ToUInt16($imageBytes, 26)
            $resource = [BitConverter]::ToUInt32($imageBytes, 4)
            Write-Output ("  texture={0} hash=0x{1:X8} chars={2:X2}/{3:X2} sampler={4} semantic={5} image=0x{6:X8} name='{7}' imageSemantic={8} noPicmip={9} category={10} size={11}x{12} resource=0x{13:X8}" -f
                $textureIndex, $textureHash, $definition[4], $definition[5],
                $definition[6], $definition[7], $image, $imageName,
                $imageBytes[11], $imageBytes[10], $imageBytes[30],
                $width, $height, $resource)
        }

        if ($seen.Count -ge $MaterialLimit) {
            break
        }
    }
}
finally {
    [void][ProcessMemory]::CloseHandle($handle)
}
