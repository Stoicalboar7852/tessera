<#
.SYNOPSIS
  Helpers shared by the elevated test harnesses (Run-MultiStreamTest.ps1, Run-QtClientTest.ps1); dot-source it.

.DESCRIPTION
  The functions read these variables from the calling script: $Stage (the protected copy of what the run executes),
  $OutDir (the run folder), $WebUser, $WebPass and $Pin (the host test instance's web credentials and pairing PIN),
  and add to $script:Checks.
#>

function Log([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg) }
function Quote([string]$s) { '"' + $s + '"' }

# Keeps the PC and its displays awake, and turns QuickEdit off (a click in the console would pause the run) unless
# the console is for a person.
function Set-Unattended([switch]$KeepQuickEdit) {
    Add-Type -Namespace Tessera -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int which);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out uint mode);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, uint mode);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);
'@
    [void][Tessera.Native]::SetThreadExecutionState([uint32]2147483651)  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
    # Screen bounds in pixels on every monitor (per-monitor aware v2; system aware before Windows 10 1703)
    if (-not [Tessera.Native]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))) { [void][Tessera.Native]::SetProcessDPIAware() }
    $in = [Tessera.Native]::GetStdHandle(-10)
    $mode = [uint32]0
    if (-not $KeepQuickEdit -and [Tessera.Native]::GetConsoleMode($in, [ref]$mode)) {
        [void][Tessera.Native]::SetConsoleMode($in, [uint32](($mode -bor 0x80) -band (-bnot 0x40)))  # QuickEdit off
    }
}

function Invoke-Tool([string]$exe, [string[]]$arguments, [string]$log, [int]$timeoutMinutes = 10, [string]$workDir = (Get-Location).Path) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WorkingDirectory $workDir
    $null = $p.Handle  # Windows PowerShell 5.1 loses the exit code without this
    if (-not $p.WaitForExit($timeoutMinutes * 60 * 1000)) { $p.Kill(); return -999 }
    return $p.ExitCode
}

# An empty $Stage that only Administrators and SYSTEM may change, so nothing unelevated can alter what the run executes
function New-ProtectedStage {
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
}

# The signed driver package into $Stage\package, refusing it unless a trusted publisher signed it
function Copy-DriverPackage([string]$projectRoot) {
    New-Item -ItemType Directory -Force (Join-Path $Stage 'package') | Out-Null
    Copy-Item (Join-Path $projectRoot 'build\vdd\x64\Release\package\*') (Join-Path $Stage 'package')
    foreach ($f in 'TesseraVdd.dll', 'tesseravdd.cat') {
        $sig = Get-AuthenticodeSignature (Join-Path $Stage "package\$f")
        $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object Thumbprint -eq $sig.SignerCertificate.Thumbprint
        if ($sig.Status -ne 'Valid' -or -not $trusted) { throw "$f is not signed by a trusted publisher ($($sig.Status))" }
    }
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
function Complete-Pairing([string]$exe, [string[]]$arguments, [string]$name, [string]$log, [string]$workDir = (Get-Location).Path) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WorkingDirectory $workDir
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
    if ($p.WaitForExit(90000)) { return $p.ExitCode }
    # Moonlight's command-line pairing keeps its window open after success: the host's client list decides
    $p.Kill()
    $clients = (Invoke-WebApi GET '/api/clients/list' '') | ConvertFrom-Json
    if (@($clients.named_certs | Where-Object { $_.name -eq $name }).Count -gt 0) { return 0 }
    return -999
}

# --- Displays and animation --------------------------------------------------------------------------------------

# Asked fresh every time (Windows Forms' Screen list is cached and only refreshes with a message loop).
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace Tessera {
  public static class Monitors {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFOEX { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }
    delegate bool EnumProc(IntPtr monitor, IntPtr hdc, ref RECT rect, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc proc, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFOEX info);
    public static string[] List() {
      var list = new List<string>();
      EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate (IntPtr monitor, IntPtr hdc, ref RECT rect, IntPtr data) {
        var info = new MONITORINFOEX();
        info.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
        if (GetMonitorInfo(monitor, ref info)) {
          var r = info.rcMonitor;
          list.Add(string.Format("{0}|{1}|{2}|{3}|{4}|{5}", info.szDevice, r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top, (info.dwFlags & 1) != 0));
        }
        return true;
      }, IntPtr.Zero);
      return list.ToArray();
    }
  }
}
'@

function Get-Screens {
    [Tessera.Monitors]::List() | ForEach-Object {
        $f = $_ -split '\|'
        @{ name = $f[0]; x = [int]$f[1]; y = [int]$f[2]; w = [int]$f[3]; h = [int]$f[4]; primary = ($f[5] -eq 'True') }
    }
}

# Callers wrap the result in @(): PowerShell returns an empty list as $null.
function Wait-NewScreens([string[]]$before, [int]$count, [int]$timeoutSeconds = 30, $client) {
    for ($i = 0; $i -lt $timeoutSeconds * 4; $i++) {
        $new = @(Get-Screens | Where-Object { $before -notcontains $_.name } | Sort-Object { $_.x })
        if ($new.Count -ge $count) { return $new }
        if ($client -and $client.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    return @(Get-Screens | Where-Object { $before -notcontains $_.name } | Sort-Object { $_.x })
}

# Animated content on each screen: 'worst' and 'typical' draw GDI test windows ('typical' animates only the first),
# 'video' plays an ffplay test pattern through Direct3D at that screen's entry in $rates (default 60 fps).
function Start-Animations($screens, [string]$mode, [string]$dir, [int[]]$rates = @()) {
    $procs = @()
    $screens = @($screens | Where-Object { $_ })
    for ($i = 0; $i -lt $screens.Count; $i++) {
        $s = $screens[$i]
        if ($mode -eq 'video') {
            # Video-like content: ffplay renders a test pattern through Direct3D, scaled to the display
            $rate = if ($i -lt $rates.Count -and $rates[$i] -gt 0) { $rates[$i] } else { 60 }
            $vf = "testsrc2=size=960x540:rate=$rate,drawtext=text='Stream $i  $($s.name -replace '\\', '/')':fontfile='C\:/Windows/Fonts/segoeuib.ttf':fontsize=36:fontcolor=white:x=20:y=20"
            $arguments = @('-hide_banner', '-loglevel', 'error', '-stats', '-f', 'lavfi', '-i', (Quote $vf), '-noborder', '-alwaysontop',
                '-left', $s.x, '-top', $s.y, '-x', $s.w, '-y', $s.h, '-an', '-window_title', (Quote "Stream $i"))
            $procs += Start-Process (Join-Path $Stage 'ffmpeg\ffplay.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $dir "anim$i.err")
            continue
        }
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Quote (Join-Path $Stage 'Show-AnimatedWindow.ps1')),
            '-X', $s.x, '-Y', $s.y, '-Width', $s.w, '-Height', $s.h, '-Label', (Quote "Stream $i  $($s.name)"),
            '-LogFile', (Quote (Join-Path $dir "anim$i.log")))
        if ($mode -eq 'typical' -and $i -gt 0) { $arguments += '-Static' }
        $procs += Start-Process powershell.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    }
    return $procs
}

function Stop-Animations($procs) { foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }

# The frame rate ffplay kept up on screen $i: the test pattern's rate minus the frames it dropped per second.
function Get-VideoContentFps([string]$dir, [int]$i, [int]$rate = 60) {
    # ffplay's last status line: "<seconds> M-V: <drift> fd= <frames dropped> ..."
    $text = Get-Content -Raw (Join-Path $dir "anim$i.err") -ErrorAction SilentlyContinue
    if (-not $text) { return $null }
    $m = [regex]::Matches($text, '([\d.]+)\s+M-V:\s*[-\d.]+\s+fd=\s*(\d+)')
    if (-not $m.Count) { return $null }
    $last = $m[$m.Count - 1]
    return [int][Math]::Round($rate - [int]$last.Groups[2].Value / [Math]::Max(1.0, [double]::Parse($last.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)))
}

function Test-Topology([string]$label) {
    $rc = Invoke-Tool (Join-Path $Stage 'topoctl.exe') @('compare', (Quote (Join-Path $OutDir 'topology-before.json'))) (Join-Path $OutDir "topology-$label.log") 1
    Log "  topology $label : $(if ($rc -eq 0) { 'as before' } else { 'DIFFERENT (see log)' })"
    return $rc -eq 0
}

function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $script:Checks += "$(if ($ok) { 'PASS' } else { 'FAIL' }) $name$(if ($detail) { ': ' + $detail })"
    Log "  $(if ($ok) { 'PASS' } else { 'FAIL' }) $name $detail"
}
