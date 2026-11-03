<#
.SYNOPSIS
  Runs the Tessera host on the dev PC for the Phase 3 acceptance tests with clients on the local network
  (docs/tests/phase-3-acceptance.md). Run elevated; type q (or press Ctrl+C) to stop and clean up.

.DESCRIPTION
  Stages the host (stage\phase2\host, the CI build) and the native tools in %ProgramData%\Tessera\lan-test, saves the
  display topology, installs the virtual display driver, adds inbound firewall rules for the host (group "Tessera LAN
  test": the streaming ports, from the local subnet only; the web UI stays reachable from this PC only), and starts
  the host with its own config and log under the run folder (logs\lan-test\<time>). Then it asks for the PINs that
  Moonlight shows when pairing.

  At the end it stops the host, restores the displays (recover.exe), removes the driver and the firewall rules, and
  compares the topology with the saved one.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\tests\Start-LanTestHost.ps1"   # elevated
#>
param(
    [string]$ProjectRoot,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot ('logs\lan-test\' + (Get-Date -Format 'yyyyMMdd-HHmm')) }
$Stage = Join-Path $env:ProgramData 'Tessera\lan-test'
$WebUser = 'tessera-test'
$WebPass = [guid]::NewGuid().ToString('N')  # this run only; shown in this window, kept in the run's host config
$Pin = ''
$RuleGroup = 'Tessera LAN test'

. (Join-Path $PSScriptRoot 'TesseraTest.ps1')

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Run this from PowerShell opened as administrator (the virtual display driver and the firewall need it).'
    exit 1
}

function Initialize-Stage {
    New-ProtectedStage
    $tools = Join-Path $ProjectRoot 'build\native-mingw\tools'
    foreach ($t in 'vdd-install\vdd-install.exe', 'recover\recover.exe', 'recover\topoctl.exe') { Copy-Item (Join-Path $tools $t) $Stage }
    Copy-DriverPackage $ProjectRoot
    Copy-Item -Recurse (Join-Path $ProjectRoot 'stage\phase2\host') (Join-Path $Stage 'host')
}

# Inbound to the host from the local network: HTTPS and HTTP (pairing, launch), RTSP, and video, control, audio and
# microphone (every display's stream uses the one video port)
function Add-FirewallRules {
    $exe = Join-Path $Stage 'host\sunshine.exe'
    $common = @{ Group = $RuleGroup; Direction = 'Inbound'; Action = 'Allow'; Program = $exe; RemoteAddress = 'LocalSubnet'; Profile = 'Any' }
    New-NetFirewallRule @common -DisplayName "$RuleGroup (TCP)" -Protocol TCP -LocalPort 47984, 47989, 48010 | Out-Null
    New-NetFirewallRule @common -DisplayName "$RuleGroup (UDP)" -Protocol UDP -LocalPort 47998, 47999, 48000, 48002, 48010 | Out-Null
}

function Remove-FirewallRules {
    try {
        Get-NetFirewallRule -Group $RuleGroup -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        # And the rules Windows adds if it asked whether to let the staged host through and someone answered
        $exe = Join-Path $Stage 'host\sunshine.exe'
        Get-NetFirewallApplicationFilter | Where-Object { $_.Program -eq $exe } | Get-NetFirewallRule | Remove-NetFirewallRule
    } catch { Log "could not remove every firewall rule: $_" }
}

# Waits for the pairing request Moonlight sent with this PIN and answers it
function Complete-LanPairing([string]$pin, [string]$name) {
    for ($i = 0; $i -lt 30; $i++) {
        $pending = (Invoke-WebApi GET '/api/pin' '') | ConvertFrom-Json
        if ($pending.pairings -and @($pending.pairings).Count -gt 0) {
            $id = @($pending.pairings)[0].id
            $r = (Invoke-WebApi POST '/api/pin' (@{ pairing_id = $id; pin = $pin; name = $name } | ConvertTo-Json -Compress)) | ConvertFrom-Json
            return [bool]$r.status
        }
        Start-Sleep -Seconds 1
    }
    Write-Host '  No pairing request arrived in 30 s: in Moonlight, click the PC again to get a new PIN.'
    return $false
}

function Show-Clients {
    $clients = (Invoke-WebApi GET '/api/clients/list' '') | ConvertFrom-Json
    if (-not @($clients.named_certs).Count) { Write-Host '  No clients paired yet.'; return }
    foreach ($c in @($clients.named_certs)) {
        Write-Host ("  {0}: multi-display {1}" -f $c.name, $(if ($c.tessera_multistream -eq $false) { 'off' } else { 'on' }))
    }
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
Start-Transcript -Path (Join-Path $OutDir 'lan-host.log') | Out-Null
$script:Checks = @()
$hostInstance = $null
$driver = $false
try {
    Set-Unattended -KeepQuickEdit
    Log 'staging'
    Initialize-Stage
    if ((Invoke-Tool (Join-Path $Stage 'topoctl.exe') @('save', (Quote (Join-Path $OutDir 'topology-before.json'))) (Join-Path $OutDir 'topology-save.log') 1) -ne 0) { throw 'could not save the display topology' }
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('install', (Quote (Join-Path $Stage 'package\TesseraVdd.inf'))) (Join-Path $OutDir 'vdd-install.log') 5
    $driver = $true
    Log "virtual display driver installed (exit $rc)"
    Add-FirewallRules
    Log "firewall rules added (group '$RuleGroup', local subnet only)"
    $hostInstance = Start-HostInstance 'lan' (Join-Path $Stage 'host')

    $addresses = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' -and $_.InterfaceAlias -notlike 'vEthernet*' })
    Write-Host ''
    Write-Host "Host '$env:COMPUTERNAME' is running."
    foreach ($a in $addresses) {
        $category = (Get-NetConnectionProfile -InterfaceIndex $a.InterfaceIndex -ErrorAction SilentlyContinue).NetworkCategory
        Write-Host "  Address: $($a.IPAddress) ($($a.InterfaceAlias), $category network)"
    }
    Write-Host '  If Moonlight does not list the PC, add it with the + button and this address.'
    Write-Host "  Web UI (this PC only): https://localhost:47990  user $WebUser  password $WebPass"
    Write-Host "  Log: $($hostInstance.log)"
    Write-Host ''
    Write-Host 'Type the PIN Moonlight shows to pair a client, s to list the paired clients, or q to stop and clean up.'
    while ($true) {
        $in = (Read-Host 'PIN, s or q').Trim()
        if ($in -eq 'q') { break }
        if ($in -eq 's') { Show-Clients; continue }
        if ($in -notmatch '^\d{4}$') { Write-Host '  A PIN has 4 digits.'; continue }
        $name = (Read-Host 'Name for this client (for example windows-laptop or macbook)').Trim()
        if (-not $name) { $name = 'client' }
        if (Complete-LanPairing $in $name) { Log "  paired $name" } else { Log "  pairing $name failed: check the PIN and try again" }
    }
} catch {
    Log "ERROR: $_"
} finally {
    Log 'stopping and cleaning up'
    if ($hostInstance) { Stop-HostInstance $hostInstance }
    Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($Stage, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-FirewallRules
    Log 'firewall rules removed'
    if ($driver) {
        Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $OutDir 'recover-end.log') 5 | Out-Null
        $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('remove', '--purge') (Join-Path $OutDir 'vdd-uninstall.log') 30
        Log "virtual display driver removed (exit $rc)"
        Test-Topology 'end' | Out-Null
    }
    Log "run folder: $OutDir"
    Stop-Transcript | Out-Null
}
