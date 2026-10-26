<#
.SYNOPSIS
  Phase 0 check of the service watchdog in the test VM: journaled display changes are undone
  (a) when the service starts, (b) when the host process is gone ("crash"), and (c) after a reboot.

.DESCRIPTION
  Installs the CI-built Sunshine service (host/tools/sunshinesvc.exe, with the [fork] Tessera watchdog) as
  "SunshineService" in the guest, with no Sunshine.exe next to it. The service then behaves as if the host
  crashed straight after every start: it runs the watchdog before each relaunch attempt (every 3 s).
  Display changes are made with topoctl in the guest's interactive session (scheduled task, interactive
  logon), because the CCD API only works there.
#>
param(
    [string]$Name = 'tessera-test',
    [string]$VmRoot = 'D:\Remote streaming\vm',
    [Parameter(Mandatory)] [string]$ServiceExe,
    [Parameter(Mandatory)] [string]$ToolsDir
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VmCommon.ps1')
$c = Get-Content -Raw (Join-Path $VmRoot 'local\credentials.json') | ConvertFrom-Json
$cred = New-Object PSCredential($c.user, (ConvertTo-SecureString $c.password -AsPlainText -Force))

function Connect([int]$minutes = 20) {
    $deadline = (Get-Date).AddMinutes($minutes)
    while ($true) {
        try { return New-PSSession -VMName $Name -Credential $cred -ErrorAction Stop }
        catch { if ((Get-Date) -gt $deadline) { throw "guest unreachable: $($_.Exception.Message)" }; Start-Sleep 10 }
    }
}

$guestHelpers = {
    function Invoke-Interactive([string]$command, [int]$timeoutSec = 90) {
        $out = "C:\tessera\it-$([guid]::NewGuid().ToString('N')).txt"
        $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c $command > `"$out`" 2>&1"
        $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
        Register-ScheduledTask -TaskName 'TesseraInteractive' -Action $action -Principal $principal -Force | Out-Null
        Start-ScheduledTask -TaskName 'TesseraInteractive'
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        do { Start-Sleep -Milliseconds 500 } while ((Get-ScheduledTask -TaskName 'TesseraInteractive').State -eq 'Running' -and (Get-Date) -lt $deadline)
        $exit = (Get-ScheduledTaskInfo -TaskName 'TesseraInteractive').LastTaskResult
        Unregister-ScheduledTask -TaskName 'TesseraInteractive' -Confirm:$false
        [pscustomobject]@{ Exit = $exit; Out = (Get-Content $out -Raw -ErrorAction SilentlyContinue) }
    }
    function Journal-Entries {
        $o = & C:\tessera\recover.exe --list 2>&1 | Out-String
        if ($o -match ', (\d+) entr') { [int]$Matches[1] } else { -1 }
    }
    function Wait-JournalEmpty([int]$timeoutSec) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
            if ((Journal-Entries) -eq 0) { return [math]::Round($sw.Elapsed.TotalSeconds, 1) }
            Start-Sleep -Milliseconds 500
        }
        return -1
    }
}

$s = Connect
$results = [ordered]@{}
try {
    Invoke-Command -Session $s {
        # A service left over from an earlier run keeps sunshinesvc.exe locked.
        if (Get-Service SunshineService -ErrorAction SilentlyContinue) {
            Stop-Service SunshineService -Force -ErrorAction SilentlyContinue
            sc.exe delete SunshineService | Out-Null
            Start-Sleep 2
        }
        New-Item -ItemType Directory -Force C:\tessera\svc\tools | Out-Null
    }
    Send-VmFile $s $ServiceExe 'C:\tessera\svc\tools\sunshinesvc.exe'
    foreach ($t in 'topoctl.exe', 'recover.exe') { Send-VmFile $s (Join-Path $ToolsDir $t) "C:\tessera\$t" }

    # (a) service start
    $results.a = Invoke-Command -Session $s -ArgumentList $guestHelpers {
        param($helpers); . ([scriptblock]::Create($helpers))
        sc.exe delete SunshineService 2>&1 | Out-Null
        $save = Invoke-Interactive 'C:\tessera\topoctl.exe save C:\tessera\before.json'
        $change = Invoke-Interactive 'C:\tessera\topoctl.exe change resolution --owner watchdog-test:start'
        $changed = (Invoke-Interactive 'C:\tessera\topoctl.exe compare C:\tessera\before.json').Exit -ne 0
        $entries = Journal-Entries
        sc.exe create SunshineService binPath= "C:\tessera\svc\tools\sunshinesvc.exe" type= own start= demand | Out-Null
        sc.exe start SunshineService | Out-Null
        $secs = Wait-JournalEmpty 60
        $match = Invoke-Interactive 'C:\tessera\topoctl.exe compare C:\tessera\before.json'
        [pscustomobject]@{ change_exit = $change.Exit; changed = $changed; entries_before = $entries; restored_after_s = $secs; match = $match.Out.Trim() }
    }

    # (b) "crash": the service is running and Sunshine.exe is gone, so the watchdog runs before each relaunch.
    $results.b = Invoke-Command -Session $s -ArgumentList $guestHelpers {
        param($helpers); . ([scriptblock]::Create($helpers))
        $change = Invoke-Interactive 'C:\tessera\topoctl.exe change resolution --owner watchdog-test:crash'
        $secs = Wait-JournalEmpty 30
        $match = Invoke-Interactive 'C:\tessera\topoctl.exe compare C:\tessera\before.json'
        [pscustomobject]@{ change_exit = $change.Exit; restored_after_s = $secs; match = $match.Out.Trim() }
    }

    # (c) reboot: stop the service, make a change, set the service to start at boot, reboot the guest.
    $results.c_before = Invoke-Command -Session $s -ArgumentList $guestHelpers {
        param($helpers); . ([scriptblock]::Create($helpers))
        Stop-Service SunshineService -Force
        # sc.exe config once silently did not take effect here; set and verify instead.
        Set-Service SunshineService -StartupType Automatic
        $mode = (Get-CimInstance Win32_Service -Filter "Name='SunshineService'").StartMode
        $change = Invoke-Interactive 'C:\tessera\topoctl.exe change resolution --owner watchdog-test:reboot'
        [pscustomobject]@{ start_mode = $mode; change_exit = $change.Exit; entries = (Journal-Entries) }
    }
    if ($results.c_before.start_mode -ne 'Auto') { throw "service start mode is $($results.c_before.start_mode), not Auto" }
    Remove-PSSession $s
    Restart-VM -Name $Name -Force -Wait -For Heartbeat
    # Wait for autologon and for any first-boot servicing to finish (it can make programs fail to start).
    $deadline = (Get-Date).AddMinutes(15)
    do {
        Start-Sleep 15
        $s = Connect
        $state = Invoke-Command -Session $s { [pscustomobject]@{
            up = ((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds
            explorer = [bool](Get-Process explorer -ErrorAction SilentlyContinue)
            servicing = [bool](Get-Process TiWorker -ErrorAction SilentlyContinue) } }
        $settled = $state.explorer -and $state.up -gt 60 -and -not $state.servicing
        if (-not $settled) { Remove-PSSession $s }
    } until ($settled -or (Get-Date) -gt $deadline)
    $results.c_after = Invoke-Command -Session $s -ArgumentList $guestHelpers {
        param($helpers); . ([scriptblock]::Create($helpers))
        $secs = Wait-JournalEmpty 120
        $match = Invoke-Interactive 'C:\tessera\topoctl.exe compare C:\tessera\before.json'
        $log = Get-Content C:\ProgramData\Tessera\logs\watchdog.log -Tail 12 -ErrorAction SilentlyContinue
        # "already-restored": the guest came back with the saved layout by itself (Hyper-V resets the resolution of
        # its virtual display on reboot), so the watchdog only had to confirm it and clear the entry.
        $outcome = if ($log -match 'restored display_topology .* owner=watchdog-test:reboot') { ($log | Select-String 'owner=watchdog-test:reboot' | Select-Object -Last 1).Line } else { 'no watchdog entry for the reboot change' }
        Stop-Service SunshineService -Force; sc.exe delete SunshineService | Out-Null
        [pscustomobject]@{ restored_after_s = $secs; match = $match.Out.Trim(); outcome = $outcome; watchdog_log = ($log -join "`n") }
    }
}
finally {
    if ($s) { Remove-PSSession $s -ErrorAction SilentlyContinue }
}

foreach ($k in $results.Keys) { Write-Host "== $k"; $results[$k] | Format-List | Out-String -Width 200 | Write-Host }
$ok = $results.a.changed -and $results.a.restored_after_s -ge 0 -and $results.a.match -like 'MATCH*' -and
      $results.b.restored_after_s -ge 0 -and $results.b.match -like 'MATCH*' -and
      $results.c_before.entries -ge 1 -and $results.c_after.restored_after_s -ge 0 -and $results.c_after.match -like 'MATCH*'
Write-Host ($(if ($ok) { 'PASS: watchdog restores on service start, after a host crash, and after a reboot' } else { 'FAIL' }))
exit $(if ($ok) { 0 } else { 1 })
