<#
.SYNOPSIS
  Phase 1 check of the Tessera VDD in the Secure Boot test VM: monitors are added and removed at runtime, and the
  monitors of a handle that disappears are removed after the grace period.

.DESCRIPTION
  Over PowerShell Direct:
    1. trusts the dev certificate and installs the signed package (as Test-VddInVm.ps1 does);
    2. checks the control interface (vdd-install query: interface version 2);
    3. runs "vdd-stress run --backend tessera" in the guest's interactive session (the CCD API only works there),
       one display at a time and batched, and copies the summaries back;
    4. ownership: starts "vdd-stress hold" with 2 displays, kills it once they are active, and measures how long
       the driver takes to remove the orphaned monitors (expected: the 2 s grace period);
    5. removes the device and the driver package.
  The VM has no GPU, so Windows renders with the Microsoft Basic Render Driver; timings here say nothing about
  the dev PC, they only show the driver works. Results are written to -OutDir.
#>
param(
    [string]$Name = 'tessera-test',
    [string]$VmRoot = 'D:\Remote streaming\vm',
    [Parameter(Mandatory)] [string]$PackageDir,
    [Parameter(Mandatory)] [string]$ToolsDir,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Counts = '1,2,4',
    [int]$Cycles = 3,
    [int]$WaitMinutes = 60
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VmCommon.ps1')
$c = Get-Content -Raw (Join-Path $VmRoot 'local\credentials.json') | ConvertFrom-Json
$cred = New-Object PSCredential($c.user, (ConvertTo-SecureString $c.password -AsPlainText -Force))
New-Item -ItemType Directory -Force $OutDir | Out-Null

$deadline = (Get-Date).AddMinutes($WaitMinutes)
$session = $null
while (-not $session) {
    try { $session = New-PSSession -VMName $Name -Credential $cred -ErrorAction Stop }
    catch {
        if ((Get-Date) -gt $deadline) { throw "guest not reachable over PowerShell Direct after $WaitMinutes min: $($_.Exception.Message)" }
        Start-Sleep -Seconds 30
    }
}

$guestHelpers = {
    # Runs a command in the interactive (autologon) session through a scheduled task and waits for it.
    function Invoke-Interactive([string]$command, [int]$timeoutSec = 600, [switch]$NoWait) {
        $out = "C:\tessera\it-$([guid]::NewGuid().ToString('N')).txt"
        $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c $command > `"$out`" 2>&1"
        $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2)
        Register-ScheduledTask -TaskName 'TesseraInteractive' -Action $action -Principal $principal -Settings $settings -Force | Out-Null
        Start-ScheduledTask -TaskName 'TesseraInteractive'
        if ($NoWait) { return $out }
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        do { Start-Sleep -Milliseconds 500 } while ((Get-ScheduledTask -TaskName 'TesseraInteractive').State -eq 'Running' -and (Get-Date) -lt $deadline)
        $exit = (Get-ScheduledTaskInfo -TaskName 'TesseraInteractive').LastTaskResult
        Unregister-ScheduledTask -TaskName 'TesseraInteractive' -Confirm:$false
        [pscustomobject]@{ Exit = $exit; Out = (Get-Content $out -Raw -ErrorAction SilentlyContinue) }
    }
    # Active monitors as the driver reports them ("monitors N/16"), -1 if the query fails.
    function Get-ActiveMonitors {
        $q = (& C:\tessera\vdd-install.exe query 2>&1 | Out-String)
        if ($q -match 'monitors (\d+)/') { [int]$Matches[1] } else { -1 }
    }
}

try {
    Invoke-Command -Session $session { Remove-Item -Recurse -Force C:\tessera -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force C:\tessera | Out-Null }
    foreach ($f in Get-ChildItem $PackageDir -File) { Send-VmFile $session $f.FullName "C:\tessera\pkg\$($f.Name)" }
    foreach ($t in 'vdd-install\vdd-install.exe', 'vdd-stress\vdd-stress.exe', 'recover\recover.exe', 'recover\topoctl.exe') {
        Send-VmFile $session (Join-Path $ToolsDir $t) "C:\tessera\$(Split-Path $t -Leaf)"
    }

    $r = Invoke-Command -Session $session -ArgumentList $guestHelpers, $Counts, $Cycles {
        param($helpers, $counts, $cycles)
        . ([scriptblock]::Create($helpers))
        $r = [ordered]@{}
        $os = Get-CimInstance Win32_OperatingSystem
        $r.os = "$($os.Caption) $($os.BuildNumber)"
        $r.secure_boot = Confirm-SecureBootUEFI
        $cer = Get-ChildItem C:\tessera\pkg\*.cer | Select-Object -First 1
        foreach ($s in 'Root', 'TrustedPublisher') { certutil -f -addstore $s $cer.FullName 2>&1 | Out-Null }
        & C:\tessera\vdd-install.exe remove --purge 2>&1 | Out-Null
        $r.install = (& C:\tessera\vdd-install.exe install C:\tessera\pkg\TesseraVdd.inf 2>&1 | Out-String).Trim()
        $r.install_exit = $LASTEXITCODE
        Start-Sleep -Seconds 5
        $dev = Get-PnpDevice -PresentOnly | Where-Object HardwareID -contains 'Root\TesseraVdd'
        $r.pnp_status = if ($dev) { "$($dev.Status) problem=$($dev.Problem)" } else { 'not present' }
        $r.query = (& C:\tessera\vdd-install.exe query 2>&1 | Out-String).Trim()
        $r.layout_before = (Invoke-Interactive 'C:\tessera\topoctl.exe show').Out

        # Runtime add/remove, one at a time and batched.
        $one = Invoke-Interactive "C:\tessera\vdd-stress.exe run --backend tessera --counts $counts --cycles $cycles --hold-ms 1000 --settle-ms 300 --out C:\tessera\stress-one"
        $r.stress_one_exit = $one.Exit
        $r.stress_one_tail = (($one.Out -split "`n") | Select-Object -Last 4) -join ' | '
        $batch = Invoke-Interactive "C:\tessera\vdd-stress.exe run --backend tessera --batch --counts $counts --cycles $cycles --hold-ms 1000 --settle-ms 300 --out C:\tessera\stress-batch"
        $r.stress_batch_exit = $batch.Exit

        # Ownership: kill a process that holds 2 monitors; the driver must remove them after the grace period.
        $null = Invoke-Interactive 'C:\tessera\vdd-stress.exe hold --backend tessera --count 2 --seconds 120 --out C:\tessera\orphan' -NoWait
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ((Get-ActiveMonitors) -lt 2 -and $sw.Elapsed.TotalSeconds -lt 60) { Start-Sleep -Milliseconds 200 }
        $r.orphan_active_before_kill = Get-ActiveMonitors
        Start-Sleep -Seconds 2
        Get-Process vdd-stress -ErrorAction SilentlyContinue | Stop-Process -Force
        $sw.Restart()
        while ((Get-ActiveMonitors) -gt 0 -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 100 }
        $r.orphan_removed_after_s = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        $r.orphan_active_after = Get-ActiveMonitors
        Unregister-ScheduledTask -TaskName 'TesseraInteractive' -Confirm:$false -ErrorAction SilentlyContinue

        $r.layout_after = (Invoke-Interactive 'C:\tessera\topoctl.exe show').Out
        $r.wudfhost_crashes = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error' } -MaxEvents 20 -ErrorAction SilentlyContinue |
            Where-Object { $_.Message -match 'WUDFHost|TesseraVdd' } | ForEach-Object { $_.TimeCreated.ToString('s') + ' ' + (($_.Message -split "`n")[0]) })
        $r.umdf_events = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-DriverFrameworks-UserMode' } -MaxEvents 20 -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -in 10110, 10111, 10114 } | ForEach-Object { "$($_.Id) " + (($_.Message -split "`n")[0]) })
        $r.remove = (& C:\tessera\vdd-install.exe remove --purge 2>&1 | Out-String).Trim()
        [pscustomobject]$r
    }

    foreach ($d in 'stress-one', 'stress-batch', 'orphan') {
        $files = Invoke-Command -Session $session -ArgumentList $d {
            param($d)
            Get-ChildItem "C:\tessera\$d" -File -ErrorAction SilentlyContinue | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Text = (Get-Content $_.FullName -Raw) } }
        }
        New-Item -ItemType Directory -Force (Join-Path $OutDir $d) | Out-Null
        foreach ($f in $files) { [IO.File]::WriteAllText((Join-Path (Join-Path $OutDir $d) $f.Name), $f.Text) }
    }
    $r | Format-List | Out-String -Width 220 | Tee-Object -FilePath (Join-Path $OutDir 'result.txt') | Write-Host

    $summary = { param($d) $f = Join-Path $OutDir "$d\summary.json"; if (Test-Path $f) { Get-Content -Raw $f | ConvertFrom-Json } }
    $one = & $summary 'stress-one'
    $batch = & $summary 'stress-batch'
    $opsOk = { param($s) $s -and @($s.operations | Where-Object { $_.failed -gt 0 -or $_.timeouts -gt 0 }).Count -eq 0 }
    $ok = $r.secure_boot -and "$($r.install_exit)" -eq '0' -and $r.pnp_status -like 'OK*' -and $r.query -like '*interface v2*' -and
          (& $opsOk $one) -and (& $opsOk $batch) -and $r.orphan_active_before_kill -eq 2 -and $r.orphan_active_after -eq 0 -and
          $r.orphan_removed_after_s -ge 1.5 -and $r.orphan_removed_after_s -le 6 -and @($r.wudfhost_crashes).Count -eq 0
    Write-Host ($(if ($ok) { 'PASS: monitors are added and removed at runtime; orphaned monitors are removed after the grace period' } else { 'FAIL' }))
    exit $(if ($ok) { 0 } else { 1 })
}
finally {
    if ($session) { Remove-PSSession $session }
}
