param(
    [string]$ProcessName = "JB_LiveEngine_s",
    [int]$MaterialLimit = 12
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

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$module = $process.Modules | Where-Object ModuleName -eq "jb_mp_s.dll" | Select-Object -First 1
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
    $surfaceCount = Read-U32 ($world + 32)
    $surfaces = Read-U32 ($world + 36)
    $vertexCount = Read-U32 ($world + 80)
    $vertices = Read-U32 ($world + 84)
    Write-Output ("pid={0} module=0x{1:X8} world=0x{2:X8} surfaces={3} array=0x{4:X8} vertices={5} vertexArray=0x{6:X8}" -f
        $process.Id, $moduleBase, $world, $surfaceCount, $surfaces,
        $vertexCount, $vertices)

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
        $stateSlots = [BitConverter]::ToString($materialBytes, 32, 35).Replace("-", "")
        Write-Output ("surface={0} material=0x{1:X8} name='{2}' textures={3} techset=0x{4:X8}:'{5}' defs=0x{6:X8} states={7}" -f
            $surfaceIndex, $material, $materialName, $textureCount, $techniqueSet,
            $techniqueName, $textureDefs, $stateSlots)

        for ($textureIndex = 0; $textureIndex -lt $textureCount; ++$textureIndex) {
            $definition = Read-Bytes ($textureDefs + $textureIndex * 12) 12
            $image = [BitConverter]::ToUInt32($definition, 8)
            if ($image -eq 0 -or $image -eq 0xFFFFFFFF -or $image -eq 0xFFFFFFFE) {
                Write-Output ("  texture={0} semantic={1} image=0x{2:X8}" -f
                    $textureIndex, $definition[7], $image)
                continue
            }

            $imageBytes = Read-Bytes $image 36
            $imageName = Read-CString ([BitConverter]::ToUInt32($imageBytes, 32))
            $width = [BitConverter]::ToUInt16($imageBytes, 24)
            $height = [BitConverter]::ToUInt16($imageBytes, 26)
            $resource = [BitConverter]::ToUInt32($imageBytes, 4)
            Write-Output ("  texture={0} semantic={1} image=0x{2:X8} name='{3}' size={4}x{5} resource=0x{6:X8}" -f
                $textureIndex, $definition[7], $image, $imageName, $width, $height, $resource)
        }

        if ($seen.Count -ge $MaterialLimit) {
            break
        }
    }
}
finally {
    [void][ProcessMemory]::CloseHandle($handle)
}
