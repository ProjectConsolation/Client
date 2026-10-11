[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [ValidateSet('Host','Client')][string]$Role = 'Host',
    [switch]$Dedicated,
    [ValidateRange(1,65534)][int]$Port = 0
)

$ErrorActionPreference = 'Stop'
$testRoot = (Resolve-Path -LiteralPath $GameDirectory).Path
$launcher = Join-Path $testRoot 'JB_Launcher_s.exe'
foreach ($file in @('JB_Launcher_s.exe', 'xlive.dll', 'd3d9.dll', 'jb_mp_s.dll', 'consolation/zone/common_consolation.ff')) {
    if (!(Test-Path -LiteralPath (Join-Path $testRoot $file) -PathType Leaf)) {
        throw "Missing test runtime file: $file"
    }
}
if (!$Port) { $Port = if ($Role -eq 'Host') { 1000 } else { 1002 } }
if ($Dedicated -and $Role -ne 'Host') { throw 'Dedicated is only valid for Host.' }
Write-Output 'Rebuild/deploy the matching client and local xlive shim yourself before this test.'
Write-Output 'This helper does not launch the game, modify profiles, or alter firewall rules.'
Write-Output "Gameplay UDP port: $Port; discovery UDP port: $($Port + 1)"
$hostSwitches = if ($Dedicated) { ' -dedicated -set g_gametype dm -map mp_shipment' } else { '' }
Write-Output "& '$launcher' -multiplayer -offline -offline_test -offline_test_port=$Port -name Test$Role$hostSwitches"
Write-Output 'In the game console: offlineNetStatus'
if ($Role -eq 'Host') {
    if ($Dedicated) {
        Write-Output 'Experimental native dedicated transition: graphics initialize first; no local host session member.'
        Write-Output 'Verify dedicated=1, sv_running=1, native_session_handle=1, native_session_active=1 and no host roster entry.'
    } else {
        Write-Output 'Use SYSTEM LINK / CUSTOM GAME to create and start a match with no bots. Keep the host playable.'
    }
} else {
    Write-Output 'After the host starts: connectIP 127.0.0.1:1000'
    Write-Output 'For two machines, replace 127.0.0.1 with the host LAN IPv4 address and use its selected port.'
}
Write-Output 'Verify different XUIDs, both roster entries, both controllable players, movement/fire/damage, disconnect and rejoin.'
Write-Output 'Counters or a successful session-query response alone do not prove a working gameplay connection.'
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $testRoot 'd3d9.dll'), (Join-Path $testRoot 'xlive.dll') |
    Select-Object Path,Hash
