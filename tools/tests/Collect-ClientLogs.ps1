<#
.SYNOPSIS
  Collects a Windows client's Moonlight logs and what the Phase 3 tests need to read them (monitors, GPU, network
  link) into tessera-client-logs-<time>.zip on the Desktop. Needs no administrator rights.

.PARAMETER MoonlightDir
  The portable client's folder (its logs are there); without it, only %TEMP% (an installed Moonlight's log folder).

.PARAMETER Newest
  How many of the newest Moonlight-*.log files to take (default 5).

.PARAMETER OutDir
  Where the zip goes (default: the Desktop).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File Collect-ClientLogs.ps1 -MoonlightDir "$env:USERPROFILE\Desktop\Tessera Moonlight"
#>
param(
    [string]$MoonlightDir,
    [int]$Newest = 5,
    [string]$OutDir = [Environment]::GetFolderPath('Desktop')
)

$ErrorActionPreference = 'Stop'
$name = 'tessera-client-logs-' + (Get-Date -Format 'yyyyMMdd-HHmm')
$work = Join-Path $env:TEMP $name
New-Item -ItemType Directory -Force $work | Out-Null

$dirs = @($env:TEMP)
if ($MoonlightDir) { $dirs = @($MoonlightDir) + $dirs }
$logs = @($dirs | Where-Object { Test-Path $_ } | ForEach-Object { Get-ChildItem $_ -Filter 'Moonlight-*.log' -File } |
    Sort-Object LastWriteTime -Descending | Select-Object -First $Newest)
foreach ($l in $logs) { Copy-Item $l.FullName $work }
Write-Host "logs: $($logs.Count)"

$info = @()
$info += "Collected: $(Get-Date -Format o)"
$info += "Computer: $env:COMPUTERNAME"
$os = Get-CimInstance Win32_OperatingSystem
$info += "Windows: $($os.Caption) $($os.Version)"
$info += "CPU: $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name)"
$info += "Memory: $([Math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB"
$info += ''
$info += 'GPUs:'
foreach ($g in Get-CimInstance Win32_VideoController) { $info += "  $($g.Name), driver $($g.DriverVersion), $($g.CurrentHorizontalResolution)x$($g.CurrentVerticalResolution) @ $($g.CurrentRefreshRate) Hz" }
$info += ''
$info += 'Monitors:'
Add-Type -AssemblyName System.Windows.Forms
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $info += "  $($s.DeviceName) $($s.Bounds.Width)x$($s.Bounds.Height) at ($($s.Bounds.X),$($s.Bounds.Y))$(if ($s.Primary) { ', primary' })" }
foreach ($m in Get-CimInstance -Namespace root\wmi WmiMonitorID -ErrorAction SilentlyContinue) {
    $model = -join ($m.UserFriendlyName | Where-Object { $_ } | ForEach-Object { [char]$_ })
    $info += "  model: $model"
}
$info += ''
$info += 'Network adapters (up):'
foreach ($a in Get-NetAdapter | Where-Object Status -eq 'Up') { $info += "  $($a.Name): $($a.InterfaceDescription), $($a.LinkSpeed)" }
$info += ''
$info += 'Wi-Fi:'
$info += @(& netsh.exe wlan show interfaces 2>$null | Where-Object { $_ -match '^\s+(Name|Description|State|Radio type|Band|Channel|Receive rate|Transmit rate|Signal)\s*:' } | ForEach-Object { '  ' + $_.Trim() })
$info | Set-Content -Encoding UTF8 (Join-Path $work 'system.txt')

$zip = Join-Path $OutDir "$name.zip"
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item -Recurse -Force $work
Write-Host "written: $zip"
