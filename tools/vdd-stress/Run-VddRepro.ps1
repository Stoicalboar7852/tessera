#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Reproduces the multi-display freeze with third-party VDDs (Phase 1): installs each driver, runs vdd-stress
  under an ETW trace, uninstalls it again and restores the display layout. Unattended and resumable.

.DESCRIPTION
  Run once from an elevated PowerShell. For each driver in -Drivers (default parsec, sudovda, mtt):
    1. checks every package file against third-party-vdds.json,
    2. trusts the driver's signer only if it needs it (SudoVDA: its self-signed code-signing certificate in Root +
       TrustedPublisher; MTT: its SignPath leaf certificate in TrustedPublisher), exactly as their own installers do,
    3. installs it with vdd-install --hwid,
    4. runs "vdd-stress run" (add/remove cycles with 1, 2, 4, 8 displays) and "vdd-stress hold" (steady state),
       each under WPR (GPU.light + CPU.light + tessera-display.wprp),
    5. exports display/driver events from the System and Application logs,
    6. uninstalls the driver, removes the trust and settings it added, and restores the layout saved at the start.

  Safety:
    - Everything that runs elevated is first copied to %ProgramData%\Tessera\vdd-repro (Administrators and SYSTEM
      can write there, users can only read), because the project drive is exFAT and has no access control.
    - Physical monitors stay on the whole time; the layout from before the run is restored after every driver.
    - A logon task ("Tessera VDD repro resume", highest privileges, this user only) resumes the run if the PC
      crashes or hangs. A driver that was running when the PC went down is recorded as "crashed", cleaned up and
      not retried. The task is removed when the run finishes.

  Output: <project>\logs\vdd-repro\<timestamp>\ with state.json, harness.log, results.md and per driver
  stress\*.csv, hold\*.csv, *.etl, events-*.csv.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\vdd-stress\Run-VddRepro.ps1"
#>
param(
    [string[]]$Drivers = @('parsec', 'sudovda', 'mtt'),
    [string]$Counts = '1,2,4,8',
    [int]$Cycles = 10,
    [int]$HoldCount = 4,
    [int]$HoldSeconds = 120,
    [string]$Mode = '1920x1080@60',
    [int]$BaselineSeconds = 60,
    [switch]$NoTrace,
    [string]$OutDir,
    [switch]$Resume,
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Stop'
$Stage = Join-Path $env:ProgramData 'Tessera\vdd-repro'
$TaskName = 'Tessera VDD repro resume'
$MttConfigDir = 'C:\VirtualDisplayDriver'
$MttRegKey = 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver'

# ---------------------------------------------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------------------------------------------

function Save-State($state) {
    $tmp = Join-Path $OutDir 'state.json.tmp'
    $state | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $tmp
    Move-Item -Force $tmp (Join-Path $OutDir 'state.json')
}

function Get-Step($state, [string]$id) { $state.steps | Where-Object { $_.id -eq $id } | Select-Object -First 1 }

function Set-StepStatus($state, [string]$id, [string]$status, [string]$note) {
    $s = Get-Step $state $id
    $s.status = $status
    if ($note) { $s.notes += $note }
    if ($status -eq 'running') { $s.started = (Get-Date).ToString('o') }
    if ($status -ne 'running') { $s.ended = (Get-Date).ToString('o') }
    Save-State $state
}

function Log([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg) }

# ---------------------------------------------------------------------------------------------------------------
# Staging (copy everything that runs elevated to an admin-only folder)
# ---------------------------------------------------------------------------------------------------------------

function Initialize-Stage([string]$root) {
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
    New-Item -ItemType Directory -Force $Stage | Out-Null
    # Administrators and SYSTEM: full control; Users: read and execute; no inheritance from ProgramData.
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $inherit = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    $none = [System.Security.AccessControl.PropagationFlags]::None
    foreach ($rule in @(
            @('S-1-5-32-544', 'FullControl'), @('S-1-5-18', 'FullControl'), @('S-1-5-32-545', 'ReadAndExecute'))) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($rule[0])
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sid, $rule[1], $inherit, $none, 'Allow')))
    }
    Set-Acl -Path $Stage -AclObject $acl

    $tools = Join-Path $root 'build\native-mingw\tools'
    Copy-Item (Join-Path $tools 'vdd-stress\vdd-stress.exe') $Stage
    Copy-Item (Join-Path $tools 'vdd-install\vdd-install.exe') $Stage
    foreach ($f in 'Run-VddRepro.ps1', 'third-party-vdds.json', 'tessera-display.wprp') { Copy-Item (Join-Path $PSScriptRoot $f) $Stage }

    $manifest = Get-Content -Raw (Join-Path $Stage 'third-party-vdds.json') | ConvertFrom-Json
    foreach ($d in $manifest.drivers) {
        $src = Join-Path $root "cache\vdd-repro\$($d.dir)"
        $dst = Join-Path $Stage "drivers\$($d.name)"
        New-Item -ItemType Directory -Force $dst | Out-Null
        foreach ($p in $d.files.PSObject.Properties) {
            $f = Join-Path $src $p.Name
            if (-not (Test-Path $f)) { throw "$($d.name): $f is missing; run tools\vdd-stress\Get-VddPackages.ps1 first" }
            Copy-Item $f $dst
            $h = (Get-FileHash (Join-Path $dst $p.Name) -Algorithm SHA256).Hash.ToLower()
            if ($h -ne $p.Value) { throw "$($d.name): $($p.Name) has SHA-256 $h, expected $($p.Value)" }
        }
    }
}

# ---------------------------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------------------------

function Invoke-Tool([string]$exe, [string[]]$arguments, [string]$log, [int]$timeoutMinutes = 90) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle  # cache the handle, or Windows PowerShell 5.1 loses the exit code
    if (-not $p.WaitForExit($timeoutMinutes * 60 * 1000)) {
        Log "  $([IO.Path]::GetFileName($exe)) did not finish within $timeoutMinutes min; killing it"
        $p.Kill()
        return -999
    }
    return $p.ExitCode
}

function Quote([string]$s) { '"' + $s + '"' }

$Wpr = Join-Path $env:SystemRoot 'System32\wpr.exe'

function Start-Trace([string]$name) {
    if ($NoTrace) { return $false }
    Invoke-Tool $Wpr @('-cancel') (Join-Path $OutDir 'wpr-cancel.log') 3 | Out-Null  # a session left over from a crash
    $profile = Quote ((Join-Path $Stage 'tessera-display.wprp') + '!TesseraDisplay')
    $rc = Invoke-Tool $Wpr @('-start', 'GPU.light', '-start', 'CPU.light', '-start', $profile, '-filemode') (Join-Path $OutDir 'wpr-start.log') 3
    if ($rc -ne 0) { Log "  WPR did not start ($name, exit $rc); continuing without a trace"; return $false }
    return $true
}

function Stop-Trace([bool]$started, [string]$etl) {
    if (-not $started) { return }
    $rc = Invoke-Tool $Wpr @('-stop', (Quote $etl)) (Join-Path $OutDir 'wpr-stop.log') 15
    if ($rc -ne 0) { Log "  WPR stop failed (exit $rc)" }
}

function Export-Events([datetime]$since, [string]$dir) {
    $systemProviders = 'Display', 'Microsoft-Windows-DriverFrameworks-UserMode', 'Microsoft-Windows-Kernel-PnP',
        'Microsoft-Windows-Kernel-Power', 'Microsoft-Windows-WER-SystemErrorReporting', 'BugCheck', 'EventLog',
        'nvlddmkm', 'amdkmdag', 'amdkmdap', 'Microsoft-Windows-Dxgkrnl', 'Microsoft-Windows-UserModePowerService'
    $rows = @()
    try {
        $rows += Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction Stop |
            Where-Object { $systemProviders -contains $_.ProviderName }
    } catch { }
    try {
        $rows += Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since } -ErrorAction Stop |
            Where-Object { $_.ProviderName -in 'Application Error', 'Application Hang', 'Windows Error Reporting' -and
                $_.Message -match '(?i)dwm\.exe|WUDFHost|explorer\.exe|IddCx|MttVDD|SudoVDA|mm\.dll' }
    } catch { }
    $rows | Sort-Object TimeCreated | Select-Object TimeCreated, LogName, ProviderName, Id, LevelDisplayName,
        @{ n = 'Message'; e = { ($_.Message -replace '\s+', ' ').Substring(0, [Math]::Min(400, ($_.Message -replace '\s+', ' ').Length)) } } |
        Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $dir 'events-windows.csv')
    return @($rows).Count
}

# ---------------------------------------------------------------------------------------------------------------
# Per-driver install / cleanup
# ---------------------------------------------------------------------------------------------------------------

function Install-Driver($d, $state) {
    $dir = Join-Path $Stage "drivers\$($d.name)"
    if ($d.trust_cert) {
        $cer = Join-Path $dir $d.trust_cert
        $c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
        if ($c.Thumbprint -ne $d.trust_thumbprint) { throw "$($d.name): certificate thumbprint $($c.Thumbprint) is not the pinned one" }
        foreach ($store in 'Root', 'TrustedPublisher') {
            Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
            $state.trusted += "$store|$($c.Thumbprint)"
        }
        Save-State $state
        Log "  trusted $($c.Subject) ($($c.Thumbprint)) in Root and TrustedPublisher"
    }
    if ($d.trust_publisher_thumbprint) {
        $sig = Get-AuthenticodeSignature (Join-Path $dir ($d.files.PSObject.Properties.Name | Where-Object { $_ -like '*.cat' }))
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $d.trust_publisher_thumbprint) {
            throw "$($d.name): catalog signature is $($sig.Status) by $($sig.SignerCertificate.Thumbprint); expected a valid signature by $($d.trust_publisher_thumbprint)"
        }
        $tmp = Join-Path $dir 'publisher.cer'
        [IO.File]::WriteAllBytes($tmp, $sig.SignerCertificate.RawData)
        Import-Certificate -FilePath $tmp -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
        $state.trusted += "TrustedPublisher|$($d.trust_publisher_thumbprint)"
        Save-State $state
        Log "  trusted publisher $($sig.SignerCertificate.Subject)"
    }
    if ($d.settings) {
        New-Item -ItemType Directory -Force $MttConfigDir | Out-Null
        Copy-Item (Join-Path $dir $d.settings) (Join-Path $MttConfigDir 'vdd_settings.xml') -Force
        New-Item -Force $MttRegKey | Out-Null
        Set-ItemProperty $MttRegKey -Name VDDPATH -Value $MttConfigDir
        $state.mtt_config = $true
        Save-State $state
    }
    $log = Join-Path $OutDir "$($d.name)\install.log"
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('--hwid', (Quote $d.hardware_id), 'install', (Quote (Join-Path $dir $d.inf))) $log 5
    Log "  vdd-install install: exit $rc"
    if ($rc -ne 0 -and $rc -ne 3) { throw "$($d.name): install failed (exit $rc), see $log" }
    # Wait for the device to start.
    for ($i = 0; $i -lt 60; $i++) {
        $dev = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $d.hardware_id }
        if ($dev -and $dev.Status -eq 'OK') { break }
        Start-Sleep -Milliseconds 500
    }
    $dev = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $d.hardware_id }
    $ver = if ($dev) { (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data } else { '' }
    Log "  device: $($dev.FriendlyName) status $($dev.Status) driver $ver"
    if (-not $dev -or $dev.Status -ne 'OK') { throw "$($d.name): device did not start (status $($dev.Status))" }
}

function Remove-Driver($d, $state) {
    $log = Join-Path $OutDir "$($d.name)\uninstall.log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('--hwid', (Quote $d.hardware_id), 'remove', '--purge') $log 5
    Log "  vdd-install remove --purge: exit $rc"
    foreach ($t in @($state.trusted)) {
        $store, $thumb = $t -split '\|'
        $p = "Cert:\LocalMachine\$store\$thumb"
        if (Test-Path $p) { Remove-Item $p; Log "  removed $thumb from LocalMachine\$store" }
    }
    $state.trusted = @()
    if ($state.mtt_config) {
        Remove-Item -Recurse -Force $MttConfigDir -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force 'HKLM:\SOFTWARE\MikeTheTech' -ErrorAction SilentlyContinue
        $state.mtt_config = $false
    }
    Save-State $state
    Restore-Layout
}

function Restore-Layout {
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') @('restore', (Quote (Join-Path $OutDir 'layout-before.json'))) (Join-Path $OutDir 'restore.log') 2
    Log "  layout restore: exit $rc ($((Get-Content (Join-Path $OutDir 'restore.log') -ErrorAction SilentlyContinue) -join ' '))"
}

# ---------------------------------------------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------------------------------------------

function Invoke-Baseline($state) {
    $dir = Join-Path $OutDir 'baseline'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') @('probe', '--seconds', $BaselineSeconds, '--out', (Quote $dir)) (Join-Path $dir 'probe.log') 10
    Log "  baseline probe ($BaselineSeconds s): exit $rc"
}

function Invoke-DriverRun($d, $state) {
    $dir = Join-Path $OutDir $d.name
    New-Item -ItemType Directory -Force $dir | Out-Null
    $since = Get-Date
    Install-Driver $d $state
    $common = @('--backend', $d.name, '--mode', $Mode)
    if ($d.settings) { $common += @('--mtt-config', (Quote (Join-Path $MttConfigDir 'vdd_settings.xml'))) }

    $traced = Start-Trace "$($d.name) stress"
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') (@('run') + $common + @('--counts', $Counts, '--cycles', $Cycles, '--out', (Quote (Join-Path $dir 'stress')))) (Join-Path $dir 'stress.log') 120
    Stop-Trace $traced (Join-Path $dir 'stress.etl')
    Log "  stress run: exit $rc"
    (Get-Step $state $d.name).notes += "stress exit $rc"

    $traced = Start-Trace "$($d.name) hold"
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') (@('hold') + $common + @('--count', $HoldCount, '--seconds', $HoldSeconds, '--out', (Quote (Join-Path $dir 'hold')))) (Join-Path $dir 'hold.log') ([Math]::Ceiling($HoldSeconds / 60) + 15)
    Stop-Trace $traced (Join-Path $dir 'hold.etl')
    Log "  hold run ($HoldCount displays, $HoldSeconds s): exit $rc"
    (Get-Step $state $d.name).notes += "hold exit $rc"
    Save-State $state

    $n = Export-Events $since $dir
    Log "  exported $n Windows events"
    Remove-Driver $d $state
}

function Write-Results($state) {
    $lines = @('# VDD freeze reproduction results', '', "Run: $($state.started) .. $(Get-Date -Format o)", "Machine: $($state.machine)", '',
        '| Driver | Step | Status | Add/set max (ms) | Remove max (ms) | compose max (ms) | ui max (ms) | sched max (ms) | stalls > 250 ms | WUDFHost CPU mean (%) |',
        '|---|---|---|---|---|---|---|---|---|---|')
    foreach ($s in $state.steps) {
        foreach ($kind in 'stress', 'hold', '') {
            $f = if ($kind) { Join-Path $OutDir "$($s.id)\$kind\summary.json" } else { Join-Path $OutDir "$($s.id)\summary.json" }
            if ($s.id -eq 'baseline' -and $kind -ne '') { continue }
            if ($s.id -ne 'baseline' -and $kind -eq '') { continue }
            $j = $null
            if (Test-Path $f) { $j = Get-Content -Raw $f | ConvertFrom-Json }
            $addMax = ''; $remMax = ''; $c = ''; $u = ''; $sc = ''; $over = ''; $cpu = ''
            if ($j) {
                $adds = @($j.operations | Where-Object { $_.op -in 'add', 'set_count' } | ForEach-Object { $_.visible_ms.max } | Where-Object { $null -ne $_ })
                $rems = @($j.operations | Where-Object { $_.op -eq 'remove' } | ForEach-Object { $_.visible_ms.max } | Where-Object { $null -ne $_ })
                if ($adds.Count) { $addMax = '{0:N0}' -f ($adds | Measure-Object -Maximum).Maximum }
                if ($rems.Count) { $remMax = '{0:N0}' -f ($rems | Measure-Object -Maximum).Maximum }
                $c = '{0:N0}' -f $j.probes.compose.max_ms; $u = '{0:N0}' -f $j.probes.ui.max_ms; $sc = '{0:N0}' -f $j.probes.sched.max_ms
                $over = $j.probes.compose.over_250ms + $j.probes.ui.over_250ms + $j.probes.sched.over_250ms
                if ($j.cpu_percent.'WUDFHost.exe') { $cpu = '{0:N1}' -f $j.cpu_percent.'WUDFHost.exe'.mean }
            }
            $label = if ($kind) { $kind } else { 'probe' }
            $lines += "| $($s.id) | $label | $($s.status) | $addMax | $remMax | $c | $u | $sc | $over | $cpu |"
        }
    }
    $lines += @('', 'Notes per step:')
    foreach ($s in $state.steps) { $lines += "- $($s.id): " + ($s.notes -join '; ') }
    $lines | Set-Content -Encoding UTF8 (Join-Path $OutDir 'results.md')
}

# ---------------------------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------------------------

if (-not $Resume) {
    if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
    if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot ("logs\vdd-repro\" + (Get-Date -Format 'yyyyMMdd-HHmm')) }
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    Start-Transcript -Append (Join-Path $OutDir 'harness.log') | Out-Null
    Log "staging tools and packages in $Stage"
    Initialize-Stage $ProjectRoot
    $os = Get-CimInstance Win32_OperatingSystem
    $gpus = (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) $($_.DriverVersion)" }) -join '; '
    # Every property exists from the start: after a resume the steps are PSCustomObjects, which cannot gain new ones.
    $steps = @(@{ id = 'baseline'; status = 'pending'; notes = @(); started = $null; ended = $null })
    foreach ($n in $Drivers) { $steps += @{ id = $n; status = 'pending'; notes = @(); started = $null; ended = $null } }
    $state = [pscustomobject]@{
        started = (Get-Date).ToString('o'); machine = "$($os.Caption) $($os.BuildNumber); $gpus"
        params = @{ Counts = $Counts; Cycles = $Cycles; HoldCount = $HoldCount; HoldSeconds = $HoldSeconds; Mode = $Mode; NoTrace = [bool]$NoTrace }
        steps = $steps; trusted = @(); mtt_config = $false; boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
    }
    Save-State $state
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-stress.exe') @('save', (Quote (Join-Path $OutDir 'layout-before.json'))) (Join-Path $OutDir 'layout-before.txt') 2
    if ($rc -ne 0) { throw "could not save the display layout (exit $rc)" }

    # Resume after a crash or hang: run the staged copy at this user's next logon.
    $me = "$env:USERDOMAIN\$env:USERNAME"
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ("-NoProfile -ExecutionPolicy Bypass -File " + (Quote (Join-Path $Stage 'Run-VddRepro.ps1')) + " -Resume -OutDir " + (Quote $OutDir))
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $me
    $principal = New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 6)
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
    Log "registered logon task '$TaskName' (removed at the end)"
} else {
    Start-Transcript -Append (Join-Path $OutDir 'harness.log') | Out-Null
    $state = Get-Content -Raw (Join-Path $OutDir 'state.json') | ConvertFrom-Json
    $state.trusted = @($state.trusted)
    foreach ($s in $state.steps) { $s.notes = @($s.notes) }
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    Log "resuming after a restart (boot $($boot.ToString('o')))"
    Start-Sleep -Seconds 20  # let the desktop settle after logon
}

$manifest = Get-Content -Raw (Join-Path $Stage 'third-party-vdds.json') | ConvertFrom-Json

# A step still marked "running" means the PC went down during it: record that, clean up, do not retry.
foreach ($s in $state.steps | Where-Object { $_.status -eq 'running' }) {
    $since = [datetime]$s.started
    $restarted = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime -gt $since
    $power = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Id = 41, 1001, 6008 } -ErrorAction SilentlyContinue |
        ForEach-Object { "$($_.ProviderName) $($_.Id) at $($_.TimeCreated.ToString('HH:mm:ss'))" })
    $what = if ($restarted) { 'the PC restarted during this step' } else { 'the harness stopped during this step (no restart)' }
    Set-StepStatus $state $s.id 'crashed' ("$what; " + ($(if ($power) { $power -join ', ' } else { 'no crash events logged' })))
    Log "step $($s.id): $what $($power -join ', ')"
    $d = $manifest.drivers | Where-Object { $_.name -eq $s.id }
    if ($d) {
        New-Item -ItemType Directory -Force (Join-Path $OutDir $d.name) | Out-Null
        try { Export-Events $since (Join-Path $OutDir $d.name) | Out-Null } catch { }
        try { Remove-Driver $d $state } catch { Log "  cleanup failed: $($_.Exception.Message)" }
    }
}

foreach ($s in $state.steps | Where-Object { $_.status -eq 'pending' }) {
    Log "=== $($s.id)"
    Set-StepStatus $state $s.id 'running' $null
    try {
        if ($s.id -eq 'baseline') {
            Invoke-Baseline $state
        } else {
            $d = $manifest.drivers | Where-Object { $_.name -eq $s.id }
            if (-not $d) { throw "unknown driver $($s.id)" }
            Invoke-DriverRun $d $state
        }
        Set-StepStatus $state $s.id 'done' $null
    } catch {
        Log "  FAILED: $($_.Exception.Message)"
        Set-StepStatus $state $s.id 'failed' $_.Exception.Message
        $d = $manifest.drivers | Where-Object { $_.name -eq $s.id }
        if ($d) { try { Remove-Driver $d $state } catch { Log "  cleanup failed: $($_.Exception.Message)" } }
    }
}

Write-Results $state
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
Log "done; results in $(Join-Path $OutDir 'results.md')"
Set-Content -Path (Join-Path $OutDir 'DONE') -Value (Get-Date -Format o)
Stop-Transcript | Out-Null
# The staged copy is no longer needed (it may be the script that is running now, which PowerShell allows).
Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
