#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Phase 1 acceptance on the dev PC: Tessera's VDD adds and removes 8 displays 100 times, with the physical monitors
  on and headless, against the brief's targets (each change < 3 s, no system-wide stall > 250 ms, no hang).

.DESCRIPTION
  Run once from an elevated PowerShell; unattended and resumable like Run-VddRepro.ps1. It:
    1. copies vdd-stress, vdd-install, recover and the signed driver package to %ProgramData%\Tessera\vdd-accept
       (Administrators and SYSTEM may write there, users only read) and checks the package's signer is trusted;
    2. installs the driver (vdd-install install), runs the steps below with vdd-stress, each under a WPR ring
       buffer (the .etl holds the end of the step), and exports display/driver events from the Windows logs;
    3. uninstalls the driver, restores the display layout saved at the start, and checks the state journal is
       empty (headless steps switch the physical monitors off through it);
    4. writes results.md.

  Steps (each one vdd-stress run; a step the PC crashed in is recorded as "crashed" and not retried):
    baseline           60 s probe without the driver
    per-count          1, 2, 4, 8 displays x 20 cycles, one at a time, physical monitors on
    physical-8x100     8 displays x 100 cycles, one at a time, physical monitors on        (acceptance)
    physical-8x100-b   the same, all 8 in one request
    headless-8x100     8 displays x 100 cycles, one at a time, physical monitors off      (acceptance)
    headless-8x100-b   the same, all 8 in one request
    hold-4             4 displays for 120 s (steady-state stalls and CPU)
    mode-5994          2 displays x 10 cycles at 1920x1080@59.94
    hdr                2 HDR displays x 10 cycles (only if the driver reports HDR support)
  The whole run takes about two hours. Output: <project>\logs\vdd-accept\<timestamp>\.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\vdd-stress\Run-VddAcceptance.ps1"
#>
param(
    [string]$OutDir,
    [switch]$Resume,
    [switch]$NoTrace,
    [string[]]$Steps,
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Stop'
$Stage = Join-Path $env:ProgramData 'Tessera\vdd-accept'
$TaskName = 'Tessera VDD acceptance resume'
$Wpr = Join-Path $env:SystemRoot 'System32\wpr.exe'

$AllSteps = [ordered]@{
    'baseline'         = @('probe', '--seconds', '60')
    'per-count'        = @('run', '--backend', 'tessera', '--counts', '1,2,4,8', '--cycles', '20', '--hold-ms', '1000')
    'physical-8x100'   = @('run', '--backend', 'tessera', '--counts', '8', '--cycles', '100', '--hold-ms', '1000')
    'physical-8x100-b' = @('run', '--backend', 'tessera', '--counts', '8', '--cycles', '100', '--hold-ms', '1000', '--batch')
    'headless-8x100'   = @('run', '--backend', 'tessera', '--counts', '8', '--cycles', '100', '--hold-ms', '1000', '--headless')
    'headless-8x100-b' = @('run', '--backend', 'tessera', '--counts', '8', '--cycles', '100', '--hold-ms', '1000', '--headless', '--batch')
    'hold-4'           = @('hold', '--backend', 'tessera', '--count', '4', '--seconds', '120')
    'mode-5994'        = @('run', '--backend', 'tessera', '--counts', '2', '--cycles', '10', '--mode', '1920x1080@59.94')
    'hdr'              = @('run', '--backend', 'tessera', '--counts', '2', '--cycles', '10', '--hdr')
}

function Log([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg) }
function Quote([string]$s) { '"' + $s + '"' }

function Save-State($state) {
    $tmp = Join-Path $OutDir 'state.json.tmp'
    $state | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $tmp
    Move-Item -Force $tmp (Join-Path $OutDir 'state.json')
}

function Set-Step($state, [string]$id, [string]$status, [string]$note) {
    $s = $state.steps | Where-Object { $_.id -eq $id } | Select-Object -First 1
    $s.status = $status
    if ($note) { $s.notes += $note }
    if ($status -eq 'running') { $s.started = (Get-Date).ToString('o') } else { $s.ended = (Get-Date).ToString('o') }
    Save-State $state
}

function Invoke-Tool([string]$exe, [string[]]$arguments, [string]$log, [int]$timeoutMinutes = 90) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle  # cache the handle, or Windows PowerShell 5.1 loses the exit code
    if (-not $p.WaitForExit($timeoutMinutes * 60 * 1000)) {
        Log "  $([IO.Path]::GetFileName($exe)) did not finish within $timeoutMinutes min; killing it"
        $p.Kill()
        return -999
    }
    return $p.ExitCode
}

function Start-Trace {
    if ($NoTrace) { return $false }
    Invoke-Tool $Wpr @('-cancel') (Join-Path $OutDir 'wpr-cancel.log') 3 | Out-Null
    $rc = Invoke-Tool $Wpr @('-start', 'GPU.light', '-start', 'CPU.light', '-start', (Quote ((Join-Path $Stage 'tessera-display.wprp') + '!TesseraDisplay'))) (Join-Path $OutDir 'wpr-start.log') 3
    return $rc -eq 0
}

function Stop-Trace([bool]$started, [string]$etl) {
    if ($started) { Invoke-Tool $Wpr @('-stop', (Quote $etl)) (Join-Path $OutDir 'wpr-stop.log') 15 | Out-Null }
}

function Export-Events([datetime]$since, [string]$file) {
    $providers = 'Display', 'Microsoft-Windows-DriverFrameworks-UserMode', 'Microsoft-Windows-Kernel-PnP', 'Microsoft-Windows-Kernel-Power',
        'BugCheck', 'nvlddmkm', 'amdkmdag', 'Microsoft-Windows-Dxgkrnl', 'Microsoft-Windows-WER-SystemErrorReporting'
    $rows = @()
    try { $rows += Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction Stop | Where-Object { $providers -contains $_.ProviderName } } catch { }
    try {
        $rows += Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since } -ErrorAction Stop |
            Where-Object { $_.ProviderName -in 'Application Error', 'Application Hang', 'Windows Error Reporting' -and $_.Message -match '(?i)dwm\.exe|WUDFHost|explorer\.exe|TesseraVdd|vdd-stress' }
    } catch { }
    $rows | Sort-Object TimeCreated | Select-Object TimeCreated, LogName, ProviderName, Id, LevelDisplayName, @{ n = 'Message'; e = { $m = ($_.Message -replace '\s+', ' '); $m.Substring(0, [Math]::Min(400, $m.Length)) } } |
        Export-Csv -NoTypeInformation -Encoding UTF8 $file
    return @($rows).Count
}

function Initialize-Stage([string]$root) {
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
    New-Item -ItemType Directory -Force $Stage | Out-Null
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $inherit = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    foreach ($rule in @(@('S-1-5-32-544', 'FullControl'), @('S-1-5-18', 'FullControl'), @('S-1-5-32-545', 'ReadAndExecute'))) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($rule[0])
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sid, $rule[1], $inherit, 'None', 'Allow')))
    }
    Set-Acl -Path $Stage -AclObject $acl
    $tools = Join-Path $root 'build\native-mingw\tools'
    foreach ($t in 'vdd-stress\vdd-stress.exe', 'vdd-install\vdd-install.exe', 'recover\recover.exe', 'recover\topoctl.exe') { Copy-Item (Join-Path $tools $t) $Stage }
    foreach ($f in 'Run-VddAcceptance.ps1', 'tessera-display.wprp') { Copy-Item (Join-Path $PSScriptRoot $f) $Stage }
    $pkg = Join-Path $root 'build\vdd\x64\Release\package'
    New-Item -ItemType Directory -Force (Join-Path $Stage 'package') | Out-Null
    Copy-Item (Join-Path $pkg '*') (Join-Path $Stage 'package')
    # The package must be signed by a certificate this machine already trusts (tools/setup/admin-once.ps1).
    foreach ($f in 'TesseraVdd.dll', 'tesseravdd.cat') {
        $sig = Get-AuthenticodeSignature (Join-Path $Stage "package\$f")
        $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object Thumbprint -eq $sig.SignerCertificate.Thumbprint
        if ($sig.Status -ne 'Valid' -or -not $trusted) { throw "$f is not signed by a trusted publisher ($($sig.Status)); sign it with tools\signing\Sign-DriverPackage.ps1 after admin-once.ps1" }
    }
}

function Remove-Driver {
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('remove', '--purge') (Join-Path $OutDir 'uninstall.log') 5
    Log "  vdd-install remove --purge: exit $rc"
    $rc = Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $OutDir 'recover.log') 5
    Log "  recover.exe: exit $rc"
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') @('restore', (Quote (Join-Path $OutDir 'layout-before.json'))) (Join-Path $OutDir 'restore.log') 2
    Log "  layout restore: exit $rc"
}

function Write-Results($state) {
    $lines = @('# Phase 1 acceptance results (Tessera VDD)', '', "Run: $($state.started) .. $(Get-Date -Format o)", "Machine: $($state.machine)", "Driver: $($state.driver)", '',
        'Targets (brief section 4.5): each add/remove < 3 s; no stall > 250 ms; no hang.', '',  # ASCII: PowerShell 5.1 reads this file as ANSI
        '| Step | Status | Op | Displays | Ops | Failed | Change median / p95 / max (ms) | compose / ui / sched max (ms) | Stalls > 250 ms | Targets met |',
        '|---|---|---|---|---|---|---|---|---|---|')
    foreach ($s in $state.steps) {
        $f = Join-Path $OutDir "$($s.id)\summary.json"
        if (-not (Test-Path $f)) { $lines += "| $($s.id) | $($s.status) |  |  |  |  |  |  |  |  |"; continue }
        $j = Get-Content -Raw $f | ConvertFrom-Json
        $probe = '{0:N0} / {1:N0} / {2:N0}' -f $j.probes.compose.max_ms, $j.probes.ui.max_ms, $j.probes.sched.max_ms
        $over = $j.probes.compose.over_250ms + $j.probes.ui.over_250ms + $j.probes.sched.over_250ms
        $ops = @($j.operations)
        if ($ops.Count -eq 0) { $lines += "| $($s.id) | $($s.status) | probe |  |  |  |  | $probe | $over | $($j.targets_met) |"; continue }
        foreach ($o in $ops) {
            $v = $o.visible_ms
            $change = if ($v.n) { '{0:N0} / {1:N0} / {2:N0}' -f $v.median, $v.p95, $v.max } else { '' }
            $lines += "| $($s.id) | $($s.status) | $($o.op) | $($o.count) | $($o.call_ms.n) | $($o.failed + $o.timeouts) | $change | $probe | $over | $($j.targets_met) |"
        }
    }
    $lines += @('', 'Notes:')
    foreach ($s in $state.steps) { $lines += "- $($s.id): " + ($s.notes -join '; ') }
    $lines | Set-Content -Encoding UTF8 (Join-Path $OutDir 'results.md')
}

# ---------------------------------------------------------------------------------------------------------------

if (-not $Resume) {
    if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
    if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot ("logs\vdd-accept\" + (Get-Date -Format 'yyyyMMdd-HHmm')) }
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    Start-Transcript -Append (Join-Path $OutDir 'harness.log') | Out-Null
    Log "staging tools and the signed package in $Stage"
    Initialize-Stage $ProjectRoot
    $os = Get-CimInstance Win32_OperatingSystem
    $ids = if ($Steps) { $Steps } else { @($AllSteps.Keys) }
    $state = [pscustomobject]@{
        started = (Get-Date).ToString('o'); machine = "$($os.Caption) $($os.BuildNumber); " + ((Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) $($_.DriverVersion)" }) -join '; ')
        driver = ''; installed = $false
        steps = @($ids | ForEach-Object { @{ id = $_; status = 'pending'; notes = @(); started = $null; ended = $null } })
    }
    Save-State $state
    if ((Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') @('save', (Quote (Join-Path $OutDir 'layout-before.json'))) (Join-Path $OutDir 'layout-before.txt') 2) -ne 0) { throw 'could not save the display layout' }
    $me = "$env:USERDOMAIN\$env:USERNAME"
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ("-NoProfile -ExecutionPolicy Bypass -File " + (Quote (Join-Path $Stage 'Run-VddAcceptance.ps1')) + " -Resume -OutDir " + (Quote $OutDir))
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger (New-ScheduledTaskTrigger -AtLogOn -User $me) `
        -Principal (New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Highest) `
        -Settings (New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 8)) -Force | Out-Null
    Log "registered logon task '$TaskName' (removed at the end)"
} else {
    Start-Transcript -Append (Join-Path $OutDir 'harness.log') | Out-Null
    $state = Get-Content -Raw (Join-Path $OutDir 'state.json') | ConvertFrom-Json
    foreach ($s in $state.steps) { $s.notes = @($s.notes) }
    Log 'resuming after a restart'
    Start-Sleep -Seconds 20
    foreach ($s in $state.steps | Where-Object { $_.status -eq 'running' }) {
        $since = [datetime]$s.started
        $restarted = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime -gt $since
        $power = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Id = 41, 1001, 6008 } -ErrorAction SilentlyContinue | ForEach-Object { "$($_.ProviderName) $($_.Id) at $($_.TimeCreated.ToString('HH:mm:ss'))" })
        Set-Step $state $s.id 'crashed' ("$(if ($restarted) { 'the PC restarted during this step' } else { 'the harness stopped during this step' }); $($power -join ', ')")
        Log "step $($s.id) was interrupted ($($power -join ', '))"
        # A headless step may have left the physical monitors off: recover.exe undoes the journaled change.
        Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $OutDir 'recover-resume.log') 5 | Out-Null
    }
}

# Install (again after a crash; vdd-install is idempotent).
$rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('install', (Quote (Join-Path $Stage 'package\TesseraVdd.inf'))) (Join-Path $OutDir 'install.log') 5
Log "vdd-install install: exit $rc"
Start-Sleep -Seconds 3
Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('query') (Join-Path $OutDir 'query.log') 1 | Out-Null
$state.driver = "$(Get-Content (Join-Path $OutDir 'query.log') -Raw -ErrorAction SilentlyContinue)".Trim()
Save-State $state
Log "driver: $($state.driver)"
# The hdr step fails fast (STATUS_NOT_SUPPORTED) if the driver runs on IddCx < 1.10.

foreach ($s in $state.steps | Where-Object { $_.status -eq 'pending' }) {
    Log "=== $($s.id)"
    if ($rc -ne 0 -and $rc -ne 3 -and $s.id -ne 'baseline') { Set-Step $state $s.id 'failed' "driver install failed (exit $rc)"; continue }
    Set-Step $state $s.id 'running' $null
    $dir = Join-Path $OutDir $s.id
    New-Item -ItemType Directory -Force $dir | Out-Null
    $since = Get-Date
    $traced = Start-Trace
    $exit = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') (@($AllSteps[$s.id]) + @('--out', (Quote $dir))) (Join-Path $dir 'vdd-stress.log') 120
    Stop-Trace $traced (Join-Path $dir "$($s.id).etl")
    $n = Export-Events $since (Join-Path $dir 'events-windows.csv')
    $status = switch ($exit) { 0 { 'passed' } 1 { 'missed' } default { 'failed' } }
    Set-Step $state $s.id $status "vdd-stress exit $exit; $n Windows events"
    Log "  $status (exit $exit)"
    # Safety net between steps: nothing journaled may stay behind.
    Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $dir 'recover.log') 5 | Out-Null
}

Remove-Driver
Write-Results $state
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
Log "done; results in $(Join-Path $OutDir 'results.md')"
Set-Content -Path (Join-Path $OutDir 'DONE') -Value (Get-Date -Format o)
Stop-Transcript | Out-Null
Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
