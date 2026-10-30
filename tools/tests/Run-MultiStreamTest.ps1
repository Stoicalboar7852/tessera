<#
.SYNOPSIS
  Phase 2 acceptance on the dev PC: multistream with the test client, stock Moonlight against the Tessera host, the
  test client against stock Sunshine, latency, display lifecycle and limits (docs/phase-2-design.md).

.DESCRIPTION
  Run elevated (the virtual display driver's control interface is admin-only) from the interactive session, and leave
  the PC alone for about 30 minutes. Nothing outside the run folder and the stage is changed for good: the host test
  instances use their own config folders, the virtual display driver is installed for the run and removed at the
  end, and the display topology is compared with the one saved at the start.

  Needs, under <project>\stage\phase2 (tools\tests\Prepare-MultiStreamTest.ps1 fills it):
    host\            the Tessera host build (sunshine.exe + assets) from CI
    moonlight\x\     stock Moonlight (portable)
    sunshine-stock\x\Sunshine\   stock Sunshine (portable)
    ffmpeg\x\...\bin\ffmpeg.exe  for snapshots
  and the native MinGW build (mstream-client, vdd-install, recover, topoctl) and the signed driver package.

  Cases (each a folder under the run folder with the client's report.json, snapshots and logs):
    legacy        1 display, no display list (today's session) - the latency baseline on this host
    ms2-typical   2 x 1080p60, only display 0 animated; mouse moved to each stream's centre
    ms2/ms3/ms4   2/3/4 x 1080p60, every display animated (worst case)
    mixed         2560x1440@60 + 1920x1080@59.94
    limit         host limited to 2 streams, 4 requested: expect 2 and "host-limit"
    kill          host killed during a 2-stream session: displays must go, recover.exe must restore the topology
    stock-moonlight  stock Moonlight streams Desktop from the Tessera host for 60 s
    stock-sunshine   the test client asks stock Sunshine for 2 displays: expect 1 stream that works

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\tests\Run-MultiStreamTest.ps1"   # elevated
#>
param(
    [string]$ProjectRoot,
    [string]$OutDir,
    [string[]]$Cases
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot ('logs\mstream\' + (Get-Date -Format 'yyyyMMdd-HHmm')) }
$Stage = Join-Path $env:ProgramData 'Tessera\mstream-test'
$AllCases = 'legacy', 'ms2-typical', 'ms2', 'ms3', 'ms4', 'mixed', 'limit', 'kill', 'stock-moonlight', 'stock-sunshine'
if (-not $Cases) { $Cases = $AllCases }
$WebUser = 'tessera-test'
$WebPass = [guid]::NewGuid().ToString('N')  # this run only; never written outside the run's host config
$Pin = '4826'

function Log([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg) }
function Quote([string]$s) { '"' + $s + '"' }

function Set-Unattended {
    Add-Type -Namespace Tessera -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int which);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out uint mode);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, uint mode);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
    [void][Tessera.Native]::SetThreadExecutionState([uint32]2147483651)  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
    [void][Tessera.Native]::SetProcessDPIAware()  # screen bounds in pixels
    $in = [Tessera.Native]::GetStdHandle(-10)
    $mode = [uint32]0
    if ([Tessera.Native]::GetConsoleMode($in, [ref]$mode)) {
        [void][Tessera.Native]::SetConsoleMode($in, [uint32](($mode -bor 0x80) -band (-bnot 0x40)))  # QuickEdit off
    }
}

function Invoke-Tool([string]$exe, [string[]]$arguments, [string]$log, [int]$timeoutMinutes = 10) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle  # Windows PowerShell 5.1 loses the exit code without this
    if (-not $p.WaitForExit($timeoutMinutes * 60 * 1000)) { $p.Kill(); return -999 }
    return $p.ExitCode
}

function Initialize-Stage {
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
    New-Item -ItemType Directory -Force $Stage | Out-Null
    # Only Administrators and SYSTEM may change what this elevated script runs
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $inherit = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    foreach ($rule in @(@('S-1-5-32-544', 'FullControl'), @('S-1-5-18', 'FullControl'), @('S-1-5-32-545', 'ReadAndExecute'))) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($rule[0])
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sid, $rule[1], $inherit, 'None', 'Allow')))
    }
    Set-Acl -Path $Stage -AclObject $acl

    $prep = Join-Path $ProjectRoot 'stage\phase2'
    $tools = Join-Path $ProjectRoot 'build\native-mingw\tools'
    foreach ($t in 'mstream-client\mstream-client.exe', 'vdd-install\vdd-install.exe', 'recover\recover.exe', 'recover\topoctl.exe') { Copy-Item (Join-Path $tools $t) $Stage }
    Copy-Item (Join-Path $PSScriptRoot 'Show-AnimatedWindow.ps1') $Stage
    New-Item -ItemType Directory -Force (Join-Path $Stage 'package') | Out-Null
    Copy-Item (Join-Path $ProjectRoot 'build\vdd\x64\Release\package\*') (Join-Path $Stage 'package')
    foreach ($f in 'TesseraVdd.dll', 'tesseravdd.cat') {
        $sig = Get-AuthenticodeSignature (Join-Path $Stage "package\$f")
        $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object Thumbprint -eq $sig.SignerCertificate.Thumbprint
        if ($sig.Status -ne 'Valid' -or -not $trusted) { throw "$f is not signed by a trusted publisher ($($sig.Status))" }
    }
    Copy-Item -Recurse (Join-Path $prep 'host') (Join-Path $Stage 'host')
    Copy-Item -Recurse (Join-Path $prep 'moonlight\x') (Join-Path $Stage 'moonlight')
    Copy-Item -Recurse (Join-Path $prep 'sunshine-stock\x\Sunshine') (Join-Path $Stage 'sunshine-stock')
    $ffmpeg = Get-ChildItem (Join-Path $prep 'ffmpeg\x') -Recurse -Filter ffmpeg.exe | Select-Object -First 1
    Copy-Item -Recurse $ffmpeg.Directory.FullName (Join-Path $Stage 'ffmpeg')
}

# --- Hosts -------------------------------------------------------------------------------------------------------

# A host instance with its own config, state, credentials, apps and log under the run folder.
function Start-HostInstance([string]$name, [string]$exeDir, [string[]]$overrides = @()) {
    $dir = Join-Path $OutDir "host-$name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $conf = Join-Path $dir 'sunshine.conf'
    if (-not (Test-Path $conf)) {
        $p = $dir -replace '\\', '/'
        @(
            "file_state = $p/sunshine_state.json",
            "credentials_file = $p/credentials.json",
            "file_apps = $p/apps.json",
            "pkey = $p/key.pem",
            "cert = $p/cert.pem",
            "min_log_level = debug",
            "origin_web_ui_allowed = pc"
        ) | Set-Content -Encoding ASCII $conf
        '{ "env": {}, "apps": [ { "name": "Desktop", "image-path": "desktop.png" } ] }' | Set-Content -Encoding ASCII (Join-Path $dir 'apps.json')
        Invoke-Tool (Join-Path $exeDir 'sunshine.exe') @((Quote $conf), '--creds', $WebUser, $WebPass) (Join-Path $dir 'creds.log') 2 | Out-Null
    }
    $log = Join-Path $dir ('sunshine-' + (Get-Date -Format 'HHmmss') + '.log')
    $arguments = @((Quote $conf), ("log_path=" + (Quote $log))) + $overrides
    $p = Start-Process -FilePath (Join-Path $exeDir 'sunshine.exe') -ArgumentList $arguments -WorkingDirectory $exeDir -WindowStyle Hidden -PassThru
    $null = $p.Handle
    # Ready when the web UI answers
    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Milliseconds 500
        $code = & curl.exe -sk -o NUL -w '%{http_code}' -u "${WebUser}:$WebPass" https://localhost:47990/api/clients/list
        if ($code -eq '200') { break }
        if ($p.HasExited) { throw "host $name exited at start (log: $log)" }
    }
    Log "  host $name running (pid $($p.Id), log $([IO.Path]::GetFileName($log)))"
    return @{ process = $p; log = $log; dir = $dir }
}

function Stop-HostInstance($h) {
    if ($h -and -not $h.process.HasExited) {
        Stop-Process -Id $h.process.Id -Force -ErrorAction SilentlyContinue
        $h.process.WaitForExit(20000) | Out-Null
    }
    Start-Sleep -Seconds 4  # the driver removes a dead host's displays after its 2 s grace period
}

function Invoke-WebApi([string]$method, [string]$path, [string]$json) {
    $arguments = @('-sk', '-u', "${WebUser}:$WebPass", '-X', $method, "https://localhost:47990$path")
    if ($json) {
        $body = Join-Path $env:TEMP ('tessera-api-' + [guid]::NewGuid().ToString('N') + '.json')
        [IO.File]::WriteAllText($body, $json)
        $arguments += @('-H', 'Content-Type: application/json', '--data-binary', "@$body")
        $out = & curl.exe @arguments
        Remove-Item $body -ErrorAction SilentlyContinue
        return $out
    }
    return & curl.exe @arguments
}

# Starts a pairing client and enters the PIN on the host once the request shows up.
function Complete-Pairing([string]$exe, [string[]]$arguments, [string]$name, [string]$log) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 1
        $pending = (Invoke-WebApi GET '/api/pin' '') | ConvertFrom-Json
        if ($pending.pairings -and @($pending.pairings).Count -gt 0) {
            $id = @($pending.pairings)[0].id
            $r = Invoke-WebApi POST '/api/pin' (@{ pairing_id = $id; pin = $Pin; name = $name } | ConvertTo-Json -Compress)
            Log "  PIN entered for $name : $r"
            break
        }
    }
    if (-not $p.WaitForExit(90000)) { $p.Kill(); return -999 }
    return $p.ExitCode
}

# --- Displays and animation --------------------------------------------------------------------------------------

Add-Type -AssemblyName System.Windows.Forms

function Get-Screens { [System.Windows.Forms.Screen]::AllScreens | ForEach-Object { @{ name = $_.DeviceName; x = $_.Bounds.X; y = $_.Bounds.Y; w = $_.Bounds.Width; h = $_.Bounds.Height; primary = $_.Primary } } }

function Wait-NewScreens([string[]]$before, [int]$count, [int]$timeoutSeconds = 30) {
    for ($i = 0; $i -lt $timeoutSeconds * 4; $i++) {
        $new = @(Get-Screens | Where-Object { $before -notcontains $_.name } | Sort-Object { $_.x })
        if ($new.Count -ge $count) { return $new }
        Start-Sleep -Milliseconds 250
    }
    return @(Get-Screens | Where-Object { $before -notcontains $_.name } | Sort-Object { $_.x })
}

function Start-Animations($screens, [string]$mode) {
    $procs = @()
    for ($i = 0; $i -lt @($screens).Count; $i++) {
        $s = @($screens)[$i]
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Quote (Join-Path $Stage 'Show-AnimatedWindow.ps1')),
            '-X', $s.x, '-Y', $s.y, '-Width', $s.w, '-Height', $s.h, '-Label', (Quote "Stream $i  $($s.name)"))
        if ($mode -eq 'typical' -and $i -gt 0) { $arguments += '-Static' }
        $procs += Start-Process powershell.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    }
    return $procs
}

function Stop-Animations($procs) { foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }

# --- Cases -------------------------------------------------------------------------------------------------------

function Invoke-ClientCase([string]$case, [string]$displays, [int]$seconds, [string]$animate, [string[]]$extra = @(), [string]$identity) {
    $dir = Join-Path $OutDir "cases\$case"
    New-Item -ItemType Directory -Force $dir | Out-Null
    if (-not $identity) { $identity = Join-Path $OutDir 'identity-tessera' }
    $count = @($displays -split ',').Count
    $before = @(Get-Screens | ForEach-Object { $_.name })
    $arguments = @('stream', '--host', '127.0.0.1', '--identity', (Quote $identity), '--displays', $displays, '--seconds', $seconds, '--out', (Quote $dir)) + $extra
    $client = Start-Process -FilePath (Join-Path $Stage 'mstream-client.exe') -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $dir 'client.out') -RedirectStandardError (Join-Path $dir 'client.err')
    $null = $client.Handle

    $streamScreens = @()
    if ($extra -contains '--legacy' -or $count -eq 1) {
        $streamScreens = @(Get-Screens | Where-Object { $_.primary })
    } else {
        $streamScreens = Wait-NewScreens $before $count
    }
    $streamScreens | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $dir 'screens.json')
    $anim = @()
    if ($animate -ne 'none') { $anim = Start-Animations $streamScreens $animate }

    if (-not $client.WaitForExit(($seconds + 90) * 1000)) { $client.Kill(); Log "  $case : client timed out" }
    Stop-Animations $anim
    Start-Sleep -Seconds 3  # the host removes the displays when the app is cancelled

    # Snapshots from each stream's first seconds
    Get-ChildItem $dir -Filter 'stream*.h26*' -ErrorAction SilentlyContinue | ForEach-Object {
        Invoke-Tool (Join-Path $Stage 'ffmpeg\ffmpeg.exe') @('-y', '-loglevel', 'error', '-i', (Quote $_.FullName), '-frames:v', '1', '-update', '1', (Quote ([IO.Path]::ChangeExtension($_.FullName, '.png')))) (Join-Path $dir "$($_.BaseName)-ffmpeg.log") 2 | Out-Null
    }
    $report = $null
    if (Test-Path (Join-Path $dir 'report.json')) { $report = Get-Content -Raw (Join-Path $dir 'report.json') | ConvertFrom-Json }
    $script:Results[$case] = @{ report = $report; screens = $streamScreens; exit = $client.ExitCode; expected = $count }
    $streams = if ($report) { $report.streams } else { 'n/a' }
    Log "  $case : client exit $($client.ExitCode), streams $streams"
}

function Test-Topology([string]$label) {
    $rc = Invoke-Tool (Join-Path $Stage 'topoctl.exe') @('compare', (Quote (Join-Path $OutDir 'topology-before.json'))) (Join-Path $OutDir "topology-$label.log") 1
    Log "  topology $label : $(if ($rc -eq 0) { 'as before' } else { 'DIFFERENT (see log)' })"
    return $rc -eq 0
}

# --- Results -----------------------------------------------------------------------------------------------------

function Format-Stream($s) {
    '{0}x{1} | {2:N1} | {3} | {4:N2} / {5:N2} | {6:N2} / {7:N2} | {8}' -f $s.width, $s.height, $s.fps, $s.frames,
        $s.host_latency_ms.p50, $s.host_latency_ms.p95, $s.assemble_ms.p50, $s.assemble_ms.p95, $s.rtp.fec_failed
}

function Write-Results {
    $lines = @('# Phase 2 acceptance results (multistream)', '', "Run: $script:Started .. $(Get-Date -Format o)", "Machine: $env:COMPUTERNAME", '',
        'Per stream: size | fps | frames | host processing latency p50 / p95 (ms) | assembly p50 / p95 (ms) | FEC failures', '',
        '| Case | Requested | Streams | Reason | Stream | Size | fps | Frames | Host latency p50 / p95 | Assembly p50 / p95 | FEC failed |',
        '|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($case in $Cases) {
        $r = $script:Results[$case]
        if (-not $r) { continue }
        $rep = $r.report
        if (-not $rep) { $lines += "| $case | $($r.expected) | (no report) |  |  |  |  |  |  |  |  |"; continue }
        if ($rep.error) { $lines += "| $case | $($r.expected) | error: $($rep.error) |  |  |  |  |  |  |  |  |"; continue }
        $i = 0
        foreach ($s in @($rep.per_stream)) {
            $lines += "| $case | $($r.expected) | $($rep.streams) | $($rep.launch.reason) | $i | $(Format-Stream $s) |"
            $i++
        }
    }
    $lines += @('', 'Checks:')
    foreach ($c in $script:Checks) { $lines += "- $c" }
    $lines | Set-Content -Encoding ASCII (Join-Path $OutDir 'results.md')
}

function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $script:Checks += "$(if ($ok) { 'PASS' } else { 'FAIL' }) $name$(if ($detail) { ': ' + $detail })"
    Log "  $(if ($ok) { 'PASS' } else { 'FAIL' }) $name $detail"
}

function Test-MultiCase([string]$case, [int]$expected, [double]$minFps) {
    $r = $script:Results[$case]
    if (-not $r -or -not $r.report -or $r.report.error) { Add-Check $case $false 'no report'; return }
    $rep = $r.report
    $ok = ($rep.streams -eq $expected)
    $details = @("streams $($rep.streams)")
    for ($i = 0; $i -lt [int]$rep.streams; $i++) {
        $s = @($rep.per_stream)[$i]
        $png = Join-Path $OutDir "cases\$case\stream$i.png"
        if ($s.frames -lt 1 -or $s.fps -lt $minFps -or $s.rtp.fec_failed -gt 0 -or -not (Test-Path $png)) { $ok = $false }
        $details += ('s{0}: {1:N0} fps, {2} FEC failed, snapshot {3}' -f $i, $s.fps, $s.rtp.fec_failed, $(if (Test-Path $png) { 'ok' } else { 'missing' }))
    }
    Add-Check $case $ok ($details -join '; ')
}

# --- Run ---------------------------------------------------------------------------------------------------------

New-Item -ItemType Directory -Force $OutDir | Out-Null
Start-Transcript -Path (Join-Path $OutDir 'harness.log') | Out-Null
$script:Started = Get-Date -Format o
$script:Results = @{}
$script:Checks = @()
$tessera = $null
try {
    Set-Unattended
    Log 'staging'
    Initialize-Stage
    if ((Invoke-Tool (Join-Path $Stage 'topoctl.exe') @('save', (Quote (Join-Path $OutDir 'topology-before.json'))) (Join-Path $OutDir 'topology-save.log') 1) -ne 0) { throw 'could not save the topology' }
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('install', (Quote (Join-Path $Stage 'package\TesseraVdd.inf'))) (Join-Path $OutDir 'vdd-install.log') 5
    Log "vdd-install install: exit $rc"

    Log 'Tessera host'
    $tessera = Start-HostInstance 'tessera' (Join-Path $Stage 'host')
    $rc = Complete-Pairing (Join-Path $Stage 'mstream-client.exe') @('pair', '--host', '127.0.0.1', '--pin', $Pin, '--identity', (Quote (Join-Path $OutDir 'identity-tessera'))) 'mstream-client' (Join-Path $OutDir 'pair-tessera.log')
    Add-Check 'pair test client with the Tessera host' ($rc -eq 0) "exit $rc"

    if ($Cases -contains 'legacy') { Log 'case legacy'; Invoke-ClientCase 'legacy' '1920x1080@60' 60 'worst' @('--legacy') }
    if ($Cases -contains 'ms2-typical') { Log 'case ms2-typical'; Invoke-ClientCase 'ms2-typical' '1920x1080@60,1920x1080@60' 60 'typical' @('--mouse-test') }
    if ($Cases -contains 'ms2') { Log 'case ms2'; Invoke-ClientCase 'ms2' '1920x1080@60,1920x1080@60' 60 'worst' }
    if ($Cases -contains 'ms3') { Log 'case ms3'; Invoke-ClientCase 'ms3' '1920x1080@60,1920x1080@60,1920x1080@60' 60 'worst' }
    if ($Cases -contains 'ms4') { Log 'case ms4'; Invoke-ClientCase 'ms4' '1920x1080@60,1920x1080@60,1920x1080@60,1920x1080@60' 60 'worst' }
    if ($Cases -contains 'mixed') { Log 'case mixed'; Invoke-ClientCase 'mixed' '2560x1440@60,1920x1080@59.94' 60 'worst' }
    Test-Topology 'after-multistream' | Out-Null

    if ($Cases -contains 'limit') {
        Log 'case limit (host limited to 2 streams)'
        Stop-HostInstance $tessera
        $tessera = Start-HostInstance 'tessera' (Join-Path $Stage 'host') @('tessera_max_streams=2')
        Invoke-ClientCase 'limit' '1920x1080@60,1920x1080@60,1920x1080@60,1920x1080@60' 30 'worst'
        Stop-HostInstance $tessera
        $tessera = Start-HostInstance 'tessera' (Join-Path $Stage 'host')
    }

    if ($Cases -contains 'kill') {
        Log 'case kill (host killed mid-session)'
        $before = @(Get-Screens | ForEach-Object { $_.name })
        $dir = Join-Path $OutDir 'cases\kill'
        New-Item -ItemType Directory -Force $dir | Out-Null
        $client = Start-Process -FilePath (Join-Path $Stage 'mstream-client.exe') -ArgumentList @('stream', '--host', '127.0.0.1', '--identity', (Quote (Join-Path $OutDir 'identity-tessera')), '--displays', '1920x1080@60,1920x1080@60', '--seconds', '60', '--out', (Quote $dir)) -PassThru -WindowStyle Hidden
        $new = Wait-NewScreens $before 2
        Start-Sleep -Seconds 10
        Stop-Process -Id $tessera.process.Id -Force
        Start-Sleep -Seconds 8
        $left = @(Get-Screens | Where-Object { $before -notcontains $_.name })
        Add-Check 'kill: virtual displays removed after the host died' ($new.Count -eq 2 -and $left.Count -eq 0) "created $($new.Count), left $($left.Count)"
        $rc = Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $dir 'recover.log') 5
        Add-Check 'kill: recover.exe restores the journaled topology' (($rc -eq 0) -and (Test-Topology 'after-kill')) "recover exit $rc"
        if (-not $client.WaitForExit(90000)) { $client.Kill() }
        $tessera = Start-HostInstance 'tessera' (Join-Path $Stage 'host')
    }

    if ($Cases -contains 'stock-moonlight') {
        Log 'case stock-moonlight'
        $dir = Join-Path $OutDir 'cases\stock-moonlight'
        New-Item -ItemType Directory -Force $dir | Out-Null
        $moonlight = Join-Path $Stage 'moonlight\Moonlight.exe'
        $rc = Complete-Pairing $moonlight @('pair', '127.0.0.1', '--pin', $Pin) 'Moonlight' (Join-Path $dir 'pair.log')
        Add-Check 'stock Moonlight pairs' ($rc -eq 0) "exit $rc"
        $since = (Get-Item $tessera.log).Length
        $m = Start-Process -FilePath $moonlight -ArgumentList @('stream', '127.0.0.1', 'Desktop', '--display-mode', 'windowed', '--resolution', '1280x720', '--fps', '60', '--bitrate', '10000') -PassThru
        Start-Sleep -Seconds 60
        Invoke-Tool $moonlight @('quit', '127.0.0.1') (Join-Path $dir 'quit.log') 2 | Out-Null
        if (-not $m.WaitForExit(20000)) { Stop-Process -Id $m.Id -Force -ErrorAction SilentlyContinue }
        $fs = [IO.File]::Open($tessera.log, 'Open', 'Read', 'ReadWrite')
        $fs.Seek($since, 'Begin') | Out-Null
        $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
        $fs.Close()
        $text | Set-Content -Encoding UTF8 (Join-Path $dir 'host-log-excerpt.txt')
        $connected = $text -match 'CLIENT CONNECTED'
        $multistream = $text -match 'Multistream:'
        Add-Check 'stock Moonlight streams from the Tessera host as from upstream' ($connected -and -not $multistream) "client connected: $connected, multistream used: $multistream"
    }

    Stop-HostInstance $tessera
    $tessera = $null

    if ($Cases -contains 'stock-sunshine') {
        Log 'case stock-sunshine'
        $stock = Start-HostInstance 'stock' (Join-Path $Stage 'sunshine-stock')
        $identity = Join-Path $OutDir 'identity-stock'
        $rc = Complete-Pairing (Join-Path $Stage 'mstream-client.exe') @('pair', '--host', '127.0.0.1', '--pin', $Pin, '--identity', (Quote $identity)) 'mstream-client' (Join-Path $OutDir 'pair-stock.log')
        Add-Check 'pair test client with stock Sunshine' ($rc -eq 0) "exit $rc"
        Invoke-ClientCase 'stock-sunshine' '1920x1080@60,1920x1080@60' 30 'worst' @() $identity
        Stop-HostInstance $stock
        $r = $script:Results['stock-sunshine'].report
        Add-Check 'test client falls back to one stream with stock Sunshine' ($r -and $r.streams -eq 1 -and -not $r.launch.streams -and @($r.per_stream)[0].frames -gt 0) "streams $($r.streams), frames $(@($r.per_stream)[0].frames)"
    }

    if ($script:Results['legacy']) { Test-MultiCase 'legacy' 1 50 }
    foreach ($c in @(@('ms2-typical', 2), @('ms2', 2), @('ms3', 3), @('ms4', 4), @('mixed', 2))) { if ($script:Results[$c[0]]) { Test-MultiCase $c[0] $c[1] 50 } }
    if ($script:Results['limit']) {
        $r = $script:Results['limit'].report
        Add-Check 'limit: 4 requested, host allows 2' ($r -and $r.streams -eq 2 -and $r.launch.reason -eq 'host-limit') "streams $($r.streams), reason $($r.launch.reason)"
    }
    if ($script:Results['ms2-typical'] -and $script:Results['ms2-typical'].report.mouse) {
        $screens = @($script:Results['ms2-typical'].screens)
        $ok = $true
        $detail = @()
        foreach ($m in @($script:Results['ms2-typical'].report.mouse)) {
            $s = $screens[[int]$m.stream]
            $cx = $s.x + $s.w / 2
            $cy = $s.y + $s.h / 2
            $inside = [Math]::Abs($m.cursor[0] - $cx) -le 2 -and [Math]::Abs($m.cursor[1] - $cy) -le 2
            if (-not $inside) { $ok = $false }
            $detail += "stream $($m.stream): cursor ($($m.cursor[0]),$($m.cursor[1])), centre ($cx,$cy)"
        }
        Add-Check 'mouse lands at each stream''s centre' $ok ($detail -join '; ')
    }
} catch {
    Log "ERROR: $_"
    $script:Checks += "FAIL harness: $_"
} finally {
    if ($tessera) { Stop-HostInstance $tessera }
    Get-Process -Name sunshine, Moonlight, mstream-client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Log 'cleaning up'
    Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $OutDir 'recover-end.log') 5 | Out-Null
    $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('remove', '--purge') (Join-Path $OutDir 'vdd-uninstall.log') 30
    Log "vdd-install remove --purge: exit $rc"
    $script:Checks += "INFO topology at the end: $(if (Test-Topology 'end') { 'as before' } else { 'different' })"
    Write-Results
    Log "done; results in $(Join-Path $OutDir 'results.md')"
    'done' | Set-Content (Join-Path $OutDir 'DONE')
    Stop-Transcript | Out-Null
}
