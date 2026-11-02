<#
.SYNOPSIS
  Phase 3 tests on the dev PC: the Tessera Qt client streams several displays from a Tessera host on the same PC
  (docs/phase-3-design.md; the tests on real client machines are in docs/tests/phase-3-acceptance.md).

.DESCRIPTION
  The client streams "all displays". This PC has only one or two monitors, so the client's other monitors are virtual
  displays that vdd-stress holds for the case ("stand-ins"); the host makes its own virtual display per stream, as it
  would for a client on another PC. Each of the host's displays plays an ffplay test pattern at the display's refresh
  rate. The client runs portable (its state in the stage), windowed, with the audio left on the host, and logs to the
  case folder. Leave the PC alone while it runs: the cases that check the mouse move the cursor themselves.

  Run elevated (the virtual display driver's control interface is admin-only) for every case, about 35 minutes with
  the default -Seconds 90. Not elevated, only the cases that need no stand-in (single, and stock-sunshine with one
  display) run: a smoke test of the client and this harness. Nothing outside the run folder and the stage is changed
  for good: the hosts use their own config folders, the driver is installed for the run and removed at the end, and
  the display topology is compared with the one saved at the start.

  Needs: the Qt client build (-ClientDir, default <project>\build\qt-client-release, from
  tools\setup\build-qt-client.ps1), <project>\stage\phase2 (tools\tests\Prepare-MultiStreamTest.ps1: the host build
  from CI, stock Sunshine, FFmpeg), the native MinGW build (vdd-install, vdd-stress, recover, topoctl) and the signed
  driver package.

  Cases (each a folder under the run folder with the client's log, the host's log excerpt and the content's logs):
    single          "This display": upstream's one-display session (ffplay in a corner of the captured monitor)
    ms2 ms3 ms4     all displays: the physical monitor(s) plus stand-ins at 1920x1080@60 up to 2/3/4 displays;
                    ms2 also checks the mouse (absolute mode)
    mixed           plus stand-ins at 2560x1440@60 and 1280x720@60; checks the mouse (scaling per stream)
    hz120 hz144 hz180  plus one stand-in at 1920x1080@120, 2560x1440@144 or 1920x1080@180: the extra stream runs
                    at the monitor's rate, with no doubling
    borderless      ms2 in borderless full screen; checks the mouse
    unplug          the stand-in goes away mid-session: the client ends the session and says why
    not-permitted   multi-display turned off for this client on the host: expect 1 stream, "not-permitted"
    limit           host limited to 2 streams, 3 displays asked for: expect 2, "host-limit"
    stock-sunshine  stock Sunshine and 2 displays: expect one display streaming

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\tests\Run-QtClientTest.ps1"   # elevated
#>
param(
    [string]$ProjectRoot,
    [string]$OutDir,
    [string[]]$Cases,
    [int]$Seconds = 90,
    [string]$ClientDir
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot ('logs\qtclient\' + (Get-Date -Format 'yyyyMMdd-HHmm')) }
if (-not $ClientDir) { $ClientDir = Join-Path $ProjectRoot 'build\qt-client-release' }
$Elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$Stage = if ($Elevated) { Join-Path $env:ProgramData 'Tessera\qtclient-test' } else { Join-Path $env:LOCALAPPDATA 'Tessera\qtclient-test' }
$AllCases = 'single', 'ms2', 'ms3', 'ms4', 'mixed', 'hz120', 'hz144', 'hz180', 'borderless', 'unplug', 'not-permitted', 'limit', 'stock-sunshine'
if (-not $Cases) { $Cases = if ($Elevated) { $AllCases } else { @('single', 'stock-sunshine') } }
$Seconds = [Math]::Max(45, $Seconds)  # the mouse check runs at 15 s, the unplug at 20 s
$WebUser = 'tessera-test'
$WebPass = [guid]::NewGuid().ToString('N')  # this run only; never written outside the run's host config
$Pin = '4826'
$ClientName = 'Tessera Qt'
$HostOverrides = @('tessera_make_primary=false')  # the stand-ins and the physical monitor keep their places

. (Join-Path $PSScriptRoot 'TesseraTest.ps1')

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
namespace Tessera {
  public static class Desktop {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    delegate bool EnumProc(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT point);

    // "x|y|width|height|title" of each visible top-level window of the process (client area, screen pixels)
    public static string[] Windows(int pid) {
      var list = new List<string>();
      EnumWindows(delegate (IntPtr hwnd, IntPtr data) {
        uint owner;
        GetWindowThreadProcessId(hwnd, out owner);
        if (owner == pid && IsWindowVisible(hwnd)) {
          var title = new StringBuilder(512);
          GetWindowText(hwnd, title, title.Capacity);
          RECT r;
          var origin = new POINT();
          if (GetClientRect(hwnd, out r) && ClientToScreen(hwnd, ref origin)) {
            list.Add(string.Format("{0}|{1}|{2}|{3}|{4}", origin.X, origin.Y, r.Right, r.Bottom, title));
          }
        }
        return true;
      }, IntPtr.Zero);
      return list.ToArray();
    }

    // Puts the cursor at (x, y) and records where it goes for `ms` milliseconds: "x,y,ms;x,y,ms;..."
    public static string Track(int x, int y, int ms) {
      var sb = new StringBuilder();
      var clock = Stopwatch.StartNew();
      SetCursorPos(x, y);
      POINT last = new POINT { X = int.MinValue };
      while (clock.ElapsedMilliseconds < ms) {
        POINT p;
        if (GetCursorPos(out p) && (p.X != last.X || p.Y != last.Y)) {
          sb.AppendFormat("{0},{1},{2};", p.X, p.Y, clock.ElapsedMilliseconds);
          last = p;
        }
        Thread.Yield();
      }
      return sb.ToString();
    }

    public static void Park(int x, int y) { SetCursorPos(x, y); }
  }
}
'@

function Initialize-Stage {
    if ($Elevated) {
        New-ProtectedStage
    } else {
        if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
        New-Item -ItemType Directory -Force $Stage | Out-Null
    }
    if (-not (Test-Path (Join-Path $ClientDir 'Moonlight.exe'))) { throw "no Moonlight.exe in $ClientDir (tools\setup\build-qt-client.ps1 builds it)" }
    $prep = Join-Path $ProjectRoot 'stage\phase2'
    $tools = Join-Path $ProjectRoot 'build\native-mingw\tools'
    foreach ($t in 'vdd-install\vdd-install.exe', 'vdd-stress\vdd-stress.exe', 'recover\recover.exe', 'recover\topoctl.exe') { Copy-Item (Join-Path $tools $t) $Stage }
    if ($Elevated) { Copy-DriverPackage $ProjectRoot }
    Copy-Item -Recurse (Join-Path $prep 'host') (Join-Path $Stage 'host')
    Copy-Item -Recurse (Join-Path $prep 'sunshine-stock\x\Sunshine') (Join-Path $Stage 'sunshine-stock')
    $ffmpeg = Get-ChildItem (Join-Path $prep 'ffmpeg\x') -Recurse -Filter ffmpeg.exe | Select-Object -First 1
    Copy-Item -Recurse $ffmpeg.Directory.FullName (Join-Path $Stage 'ffmpeg')
    Copy-Item -Recurse $ClientDir (Join-Path $Stage 'client')
    # Moonlight is portable when portable.dat is in its working directory: one state folder (identity, paired hosts,
    # settings) per host, as both hosts answer on 127.0.0.1
    foreach ($s in 'state-tessera', 'state-stock') {
        New-Item -ItemType Directory -Force (Join-Path $Stage $s) | Out-Null
        New-Item -ItemType File -Force (Join-Path $Stage "$s\portable.dat") | Out-Null
    }
}

# Sets key=value in a section of Moonlight's portable settings file (QSettings INI)
function Set-IniValue([string]$path, [string]$section, [string]$key, [string]$value) {
    $lines = if (Test-Path $path) { @(Get-Content $path) } else { @() }
    $out = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    $done = $false
    foreach ($l in $lines) {
        if ($l -match '^\[(.+)\]\s*$') {
            if ($inSection -and -not $done) { $out.Add("$key=$value"); $done = $true }
            $inSection = ($Matches[1] -eq $section)
        } elseif ($inSection -and $l.StartsWith("$key=")) {
            if (-not $done) { $out.Add("$key=$value"); $done = $true }
            continue
        }
        $out.Add($l)
    }
    if (-not $done) {
        if (-not $inSection) { $out.Add("[$section]") }
        $out.Add("$key=$value")
    }
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [IO.File]::WriteAllLines($path, $out)
}

# Text added to a log since byte offset $since (the writer keeps the file open)
function Read-LogSince([string]$path, [long]$since = 0) {
    try {
        $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        try {
            $fs.Seek([Math]::Min($since, $fs.Length), 'Begin') | Out-Null
            return (New-Object IO.StreamReader($fs)).ReadToEnd()
        } finally { $fs.Close() }
    } catch { return '' }
}

function Get-LogLength([string]$path) { if (Test-Path $path) { (Get-Item $path).Length } else { 0 } }

# What the client logged: its display plan, the host's answer, its windows and per-stream statistics
function Read-ClientLog([string]$text) {
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $r = @{ plan = @(); accepted = $null; requested = $null; reason = ''; streaming = $null; noMultistream = $false;
        windows = @{}; stats = @{}; stopped = @(); disconnected = @(); errors = @() }
    foreach ($m in [regex]::Matches($text, "Tessera: stream (\d+) is '([^']*)' at (\d+)x(\d+) (\d+)/(\d+) Hz")) {
        $r.plan += @{ stream = [int]$m.Groups[1].Value; name = $m.Groups[2].Value; w = [int]$m.Groups[3].Value; h = [int]$m.Groups[4].Value
            hz = [double]$m.Groups[5].Value / [double]$m.Groups[6].Value }
    }
    $m = [regex]::Match($text, 'Tessera: the host accepted (\d+) of (\d+) streams(?:: (\S+))?')
    if ($m.Success) { $r.accepted = [int]$m.Groups[1].Value; $r.requested = [int]$m.Groups[2].Value; $r.reason = $m.Groups[3].Value }
    $m = [regex]::Match($text, 'Tessera: the host can run \d+ stream\(s\) now')
    if ($m.Success) { $r.accepted = 1; $r.reason = 'host-limit' }
    $r.noMultistream = $text -match "Tessera: the host doesn't advertise multi-stream"
    $m = [regex]::Match($text, 'Tessera: (\d+) of (\d+) displays are streaming')
    if ($m.Success) { $r.streaming = [int]$m.Groups[1].Value }
    foreach ($m in [regex]::Matches($text, "Tessera: stream (\d+) window \d+ on display (-?\d+) \('([^']*)'\), (\d+)x(\d+)")) {
        $r.windows[[int]$m.Groups[1].Value] = @{ display = [int]$m.Groups[2].Value; name = $m.Groups[3].Value }
    }
    foreach ($m in [regex]::Matches($text, 'Tessera: stream (\d+) stopped \(([^)]*)\)')) { $r.stopped += "$($m.Groups[1].Value):$($m.Groups[2].Value)" }
    foreach ($m in [regex]::Matches($text, "Tessera: streamed display '([^']*)' \(stream (\d+)\) was disconnected")) { $r.disconnected += [int]$m.Groups[2].Value }
    # (Code 0 is the host ending the session normally)
    $r.errors = @(($text -split "`r?`n") | Where-Object { $_ -match 'SDL Error|Qt Critical|Qt Fatal' -and $_ -notmatch 'Connection terminated: 0$' })

    # Statistics: the first line of each stream covers the connection's start, so it only counts when it is the only one
    $lines = @{}
    $pattern = 'Tessera stats: stream=(\d+) size=(\d+)x(\d+) seconds=([\d.]+) received=(\d+) decoded=(\d+) rendered=(\d+) total=(\d+) network_dropped=(\d+) pacer_dropped=(\d+) received_fps=[\d.]+ rendered_fps=[\d.]+ decode_ms=([\d.]+) render_ms=([\d.]+)'
    foreach ($m in [regex]::Matches($text, $pattern)) {
        $s = [int]$m.Groups[1].Value
        if (-not $lines.ContainsKey($s)) { $lines[$s] = @() }
        $lines[$s] += , $m
    }
    foreach ($s in $lines.Keys) {
        $use = if ($lines[$s].Count -gt 1) { $lines[$s] | Select-Object -Skip 1 } else { $lines[$s] }
        $t = @{ seconds = 0.0; received = 0; decoded = 0; rendered = 0; netDropped = 0; pacerDropped = 0; decodeMs = 0.0; renderMs = 0.0 }
        foreach ($m in $use) {
            $t.w = [int]$m.Groups[2].Value
            $t.h = [int]$m.Groups[3].Value
            $t.seconds += [double]::Parse($m.Groups[4].Value, $inv)
            $t.received += [int64]$m.Groups[5].Value
            $t.decoded += [int64]$m.Groups[6].Value
            $t.rendered += [int64]$m.Groups[7].Value
            $t.netDropped += [int64]$m.Groups[9].Value
            $t.pacerDropped += [int64]$m.Groups[10].Value
            $t.decodeMs += [double]::Parse($m.Groups[11].Value, $inv) * [int64]$m.Groups[6].Value
            $t.renderMs += [double]::Parse($m.Groups[12].Value, $inv) * [int64]$m.Groups[7].Value
        }
        $t.receivedFps = if ($t.seconds -gt 0) { $t.received / $t.seconds } else { 0 }
        $t.renderedFps = if ($t.seconds -gt 0) { $t.rendered / $t.seconds } else { 0 }
        $t.decodeMs = if ($t.decoded -gt 0) { $t.decodeMs / $t.decoded } else { 0 }
        $t.renderMs = if ($t.rendered -gt 0) { $t.renderMs / $t.rendered } else { 0 }
        $r.stats[$s] = $t
    }
    return $r
}

# The client's display windows by stream: stream 0 is upstream's "<host> - Moonlight", the others "... (display N)"
function Get-StreamWindows([int]$processId) {
    $windows = @{}
    foreach ($line in [Tessera.Desktop]::Windows($processId)) {
        $f = $line -split '\|', 5
        $w = @{ x = [int]$f[0]; y = [int]$f[1]; w = [int]$f[2]; h = [int]$f[3]; title = $f[4] }
        if ($w.title -match '\(display (\d+)\)$') { $windows[[int]$Matches[1] - 1] = $w }
        elseif ($w.title -like '* - Moonlight') { $windows[0] = $w }
    }
    return $windows
}

function Test-Inside($point, $screen) {
    return $point[0] -ge $screen.x -and $point[0] -lt $screen.x + $screen.w -and $point[1] -ge $screen.y -and $point[1] -lt $screen.y + $screen.h
}

# Moves the cursor into each stream's window and looks for the host putting it at the same place on that stream's
# display (the host is this PC). Afterwards the client clamps the cursor to the edge of the video, as it left the
# window, so the first position on the host's display counts.
function Test-MouseRouting($windows, $plan, $hostScreens) {
    $result = @{ ok = $true; details = @() }
    for ($i = 0; $i -lt $hostScreens.Count; $i++) {
        $w = $windows[$i]
        $p = @($plan | Where-Object { $_.stream -eq $i })[0]
        if (-not $w -or -not $p) { $result.ok = $false; $result.details += "s${i}: no window"; continue }
        # The video region inside the window (letterboxed like StreamUtils::scaleSourceToDestinationSurface)
        $scale = [Math]::Min($w.w / $p.w, $w.h / $p.h)
        $dw = $p.w * $scale
        $dh = $p.h * $scale
        $dx = ($w.w - $dw) / 2
        $dy = ($w.h - $dh) / 2
        $tx = [int]($w.x + $dx + 0.3 * $dw)
        $ty = [int]($w.y + $dy + 0.4 * $dh)
        $h = $hostScreens[$i]
        $ex = $h.x + ($tx - $w.x - $dx) / $dw * $h.w
        $ey = $h.y + ($ty - $w.y - $dy) / $dh * $h.h
        $hit = $null
        $path = ''
        foreach ($nudge in 0, 1) {
            $path = [Tessera.Desktop]::Track($tx + $nudge, $ty, 1500)
            $points = @($path.TrimEnd(';') -split ';' | Where-Object { $_ } | ForEach-Object { $c = $_ -split ','; , @([int]$c[0], [int]$c[1]) })
            $hit = $points | Where-Object { (Test-Inside $_ $h) -and [Math]::Abs($_[0] - $ex) -le 4 -and [Math]::Abs($_[1] - $ey) -le 4 } | Select-Object -First 1
            if ($hit) { break }
        }
        if (-not $hit) { $result.ok = $false }
        $result.details += ('s{0}: window ({1},{2}) -> expected ({3:N0},{4:N0}) on {5}: {6}' -f $i, $tx, $ty, $ex, $ey, $h.name,
            $(if ($hit) { "at ($($hit[0]),$($hit[1]))" } else { "not seen; path $path" }))
    }
    $primary = @(Get-Screens | Where-Object { $_.primary })[0]
    [Tessera.Desktop]::Park($primary.x + 2, $primary.y + 2)
    return $result
}

# --- Cases -------------------------------------------------------------------------------------------------------

# Stand-ins that bring the client to $count monitors with the physical ones, all in one mode
function Get-StandIns([int]$count, [string]$mode) {
    $n = [Math]::Max(0, $count - $script:Physical.Count)
    return @(for ($i = 0; $i -lt $n; $i++) { $mode })
}

function Start-StandIns([string[]]$modes, [string]$dir) {
    $holds = @()
    $k = 0
    foreach ($g in @($modes | Group-Object)) {
        $before = @(Get-Screens | ForEach-Object { $_.name })
        $arguments = @('hold', '--backend', 'tessera', '--count', $g.Count, '--seconds', ($Seconds + 600), '--mode', $g.Name, '--out', (Quote (Join-Path $dir "hold$k")))
        # Each process numbers its displays from slot 0: a second one needs other IDs
        if ($k -gt 0) { $arguments += '--new-ids' }
        $p = Start-Process -FilePath (Join-Path $Stage 'vdd-stress.exe') -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $dir "hold$k.out") -RedirectStandardError (Join-Path $dir "hold$k.err")
        $null = $p.Handle
        $holds += $p
        $new = @(Wait-NewScreens $before $g.Count 45 $p)
        if ($new.Count -lt $g.Count) { throw "only $($new.Count) of $($g.Count) stand-ins at $($g.Name) appeared (see hold$k.out)" }
        $k++
    }
    Start-Sleep -Seconds 3  # Windows settles the new layout
    return $holds
}

function Stop-StandIns($holds) {
    foreach ($p in @($holds)) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
}

# Waits until only the physical monitors are left (the host's and the stand-ins' displays gone)
function Wait-PhysicalOnly([int]$timeoutSeconds = 30) {
    $names = @($script:Physical | ForEach-Object { $_.name })
    for ($i = 0; $i -lt $timeoutSeconds * 2; $i++) {
        if (-not @(Get-Screens | Where-Object { $names -notcontains $_.name }).Count) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

<#
  One client session. $def: mode (0 = this display, 1 = all displays), standIns (modes), display ('windowed' or
  'borderless'), mouse (check the mouse), expect (streams the host should accept; default: the client's plan),
  reason (expected reason), unplug (remove the stand-in mid-session), hostName ('tessera' or 'stock').
#>
function Invoke-QtCase([string]$case, [hashtable]$def) {
    $dir = Join-Path $OutDir "cases\$case"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $hostInfo = if ($def.hostName -eq 'stock') { $script:StockHost } else { $script:TesseraHost }
    $stateDir = Join-Path $Stage $(if ($def.hostName -eq 'stock') { 'state-stock' } else { 'state-tessera' })
    $r = @{ case = $case; def = $def; holds = @(); log = $null; content = @(); hostScreens = @(); exitedByItself = $false; mouse = $null; windows = @{} }
    $script:Results[$case] = $r
    $client = $null
    $anim = @()
    try {
        if ($def.standIns.Count -and -not $Elevated) { throw 'needs stand-in monitors, so the run must be elevated' }
        if ($def.standIns.Count) { $r.holds = Start-StandIns $def.standIns $dir }
        $clientScreens = @(Get-Screens)
        $primary = @($clientScreens | Where-Object { $_.primary })[0]
        # The order the client streams them in: the OS primary, then left to right (displayplan.cpp streamOrder)
        $r.clientScreens = @($primary) + @($clientScreens | Where-Object { -not $_.primary } | Sort-Object { $_.x }, { $_.y })
        $clientScreens | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $dir 'client-screens.json')

        Set-IniValue (Join-Path $stateDir 'Moonlight Game Streaming Project\Moonlight.ini') 'tessera' 'displays' $def.mode
        [Tessera.Desktop]::Park($primary.x + 2, $primary.y + 2)
        $before = @($clientScreens | ForEach-Object { $_.name })
        $hostSince = Get-LogLength $hostInfo.log
        $clientLog = Join-Path $dir 'moonlight.log'
        $display = if ($def.display) { $def.display } else { 'windowed' }
        $arguments = @('stream', '127.0.0.1', 'Desktop', '--display-mode', $display, '--audio-on-host', '--resolution', '1920x1080', '--fps', '60',
            $(if ($def.mouse) { '--absolute-mouse' } else { '--no-absolute-mouse' }), $(if ($case -eq 'single') { '--performance-overlay' } else { '--no-performance-overlay' }))
        $client = Start-Process -FilePath (Join-Path $Stage 'client\Moonlight.exe') -ArgumentList $arguments -PassThru -WorkingDirectory $stateDir -RedirectStandardOutput (Join-Path $dir 'moonlight.out') -RedirectStandardError $clientLog
        $null = $client.Handle
        $started = Get-Date

        # The host's displays: one per stream, or its own monitor (the client's primary) for a one-display session
        $planned = [Math]::Max(1, $r.clientScreens.Count)
        if ($def.mode -eq 0) { $planned = 1 }
        $wanted = if ($def.expect) { $def.expect } else { $planned }
        $r.hostScreens = @(Wait-NewScreens $before $wanted $(if ($wanted -gt 1) { 45 } else { 15 }) $client)
        $corner = $r.hostScreens.Count -eq 0
        if (-not $corner) {
            # The host lays its displays out as the client's monitors: the k-th from the left is the stream whose
            # monitor is k-th from the left
            $k = [Math]::Min($r.hostScreens.Count, $r.clientScreens.Count)
            $streamMonitors = @($r.clientScreens | Select-Object -First $k)
            $ranked = @($streamMonitors | Sort-Object { $_.x }, { $_.y })
            $hostSorted = @($r.hostScreens | Sort-Object { $_.x }, { $_.y })
            $r.hostScreens = @(for ($i = 0; $i -lt $k; $i++) { $hostSorted[[Array]::IndexOf($ranked, $streamMonitors[$i])] })
        } else {
            # The host captures the monitor the client's window is on: animate a corner the window leaves free
            $r.hostScreens = @(@{ name = 'corner of ' + $primary.name; x = $primary.x; y = $primary.y; w = 192; h = 108; primary = $true })
        }
        $r.hostScreens | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $dir 'host-screens.json')
        Start-Sleep -Seconds 2
        $plan = (Read-ClientLog (Read-LogSince $clientLog)).plan
        $rates = @(for ($i = 0; $i -lt $r.hostScreens.Count; $i++) {
            $p = @($plan | Where-Object { $_.stream -eq $i })[0]
            if ($p) { [int][Math]::Round($p.hz) } else { 60 }
        })
        $r.rates = $rates
        $anim = Start-Animations $r.hostScreens 'video' $dir $rates

        # Windows and the mouse once every stream renders
        Start-Sleep -Seconds ([Math]::Max(0, 15 - ((Get-Date) - $started).TotalSeconds))
        $r.windows = Get-StreamWindows $client.Id
        $saved = @{}
        foreach ($k in $r.windows.Keys) { $saved["stream $k"] = $r.windows[$k] }  # JSON keys must be strings
        $saved | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $dir 'client-windows.json')
        if ($def.mouse -and -not $corner) { $r.mouse = Test-MouseRouting $r.windows $plan $r.hostScreens }

        if ($def.unplug) {
            Start-Sleep -Seconds ([Math]::Max(0, 20 - ((Get-Date) - $started).TotalSeconds))
            Log "  $case : removing the stand-in"
            Stop-StandIns $r.holds
            $r.unplugged = Get-Date
            $client.WaitForExit(20000) | Out-Null
        } else {
            $left = $Seconds - ((Get-Date) - $started).TotalSeconds
            if ($left -gt 0 -and $client.WaitForExit([int]($left * 1000))) { Log "  $case : the client ended the session early" }
        }

        # The host ends the session; a client without an error dialog then quits by itself
        $closed = Invoke-WebApi POST '/api/apps/close' ''
        $r.exitedByItself = $client.WaitForExit(30000) -and -not $def.unplug
        if ($def.unplug) { $r.exitedByItself = $client.HasExited }
        Log "  $case : host closed the app ($closed), client $(if ($client.HasExited) { "exited ($($client.ExitCode))" } else { 'still open: error dialog?' })"
    } catch {
        Log "  $case : ERROR $_"
        $r.error = "$_"
        Invoke-WebApi POST '/api/apps/close' '' | Out-Null
    } finally {
        if ($client -and -not $client.HasExited) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        Stop-Animations $anim
        Stop-StandIns $r.holds
        if (-not (Wait-PhysicalOnly 30)) { Log "  $case : displays left over: $((@(Get-Screens) | ForEach-Object { $_.name }) -join ', ')" }
    }

    Read-LogSince $hostInfo.log $hostSince | Set-Content -Encoding UTF8 (Join-Path $dir 'host-log-excerpt.txt')
    $r.log = Read-ClientLog (Read-LogSince (Join-Path $dir 'moonlight.log'))
    $r.content = @(for ($i = 0; $i -lt $r.hostScreens.Count; $i++) { Get-VideoContentFps $dir $i $(if ($i -lt $r.rates.Count) { $r.rates[$i] } else { 60 }) })
    if (-not $r.log.stats.Count) {
        # A launch whose RTSP handshake failed stays pending on the host until its ping timeout, and the host gives
        # the next client that pending launch: wait it out so one failure doesn't fail the following cases.
        Start-Sleep -Seconds 15
    }
}

# --- Checks ------------------------------------------------------------------------------------------------------

function Test-QtCase([string]$case) {
    $r = $script:Results[$case]
    if (-not $r) { return }
    if ($r.error) { Add-Check $case $false $r.error; return }
    $log = $r.log
    $def = $r.def
    $streams = if ($null -ne $log.accepted) { $log.accepted } else { 1 }
    $expect = if ($def.expect) { $def.expect } elseif ($def.mode -eq 0) { 1 } else { $log.plan.Count }
    $details = @("plan $($log.plan.Count), accepted $streams$(if ($log.reason) { " ($($log.reason))" })")
    $ok = $streams -eq $expect
    if ($def.reason -and $log.reason -ne $def.reason) { $ok = $false; $details += "expected reason $($def.reason)" }
    if ($def.hostName -eq 'stock' -and $log.plan.Count -gt 1 -and -not $log.noMultistream) { $ok = $false; $details += 'no "doesn''t advertise multi-stream" line' }
    if ($streams -gt 1 -and $log.streaming -ne $streams) { $ok = $false; $details += "$($log.streaming) displays streaming" }
    if ($log.stopped.Count) { $details += "stopped: $($log.stopped -join ', ')" }

    if ($def.unplug) {
        $ok = $ok -and ($log.disconnected -contains 1)
        $details += "disconnect noticed: $(if ($log.disconnected.Count) { 'stream ' + ($log.disconnected -join ', ') } else { 'no' }), client $(if ($r.exitedByItself) { 'quit' } else { 'kept its error dialog open' })"
        Add-Check "$case : the client ends the session when a streamed monitor goes" $ok ($details -join '; ')
        return
    }

    # Every stream keeps up with its content and its monitor; nothing lost on the way
    for ($i = 0; $i -lt $streams; $i++) {
        $s = $log.stats[$i]
        if (-not $s) { $ok = $false; $details += "s${i}: no statistics"; continue }
        $hz = if ($i -lt $r.rates.Count) { $r.rates[$i] } else { 60 }
        $content = if ($i -lt $r.content.Count) { $r.content[$i] } else { $null }
        $target = if ($null -ne $content) { [Math]::Min($hz, $content) } else { $hz }
        $good = $s.receivedFps -ge 0.9 * $target -and $s.renderedFps -ge 0.9 * $s.receivedFps -and $s.netDropped -le 0.01 * [Math]::Max(1, $s.received)
        if (-not $good) { $ok = $false }
        $details += ('s{0}: {1}x{2}@{3} received {4:N1} rendered {5:N1} fps (content {6}), dropped {7} network / {8} pacer' -f $i, $s.w, $s.h, $hz,
            $s.receivedFps, $s.renderedFps, $(if ($null -ne $content) { $content } else { '-' }), $s.netDropped, $s.pacerDropped)
    }

    # Each display's window on its own monitor (borderless: covering it)
    for ($i = 1; $i -lt $streams; $i++) {
        $w = $r.windows[$i]
        $screen = if ($i -lt $r.clientScreens.Count) { $r.clientScreens[$i] } else { $null }
        if (-not $w -or -not $screen) { $ok = $false; $details += "s${i}: no window"; continue }
        $inside = Test-Inside @(($w.x + $w.w / 2), ($w.y + $w.h / 2)) $screen
        $covers = $w.x -eq $screen.x -and $w.y -eq $screen.y -and $w.w -eq $screen.w -and $w.h -eq $screen.h
        if (-not $inside -or ($def.display -eq 'borderless' -and -not $covers)) { $ok = $false; $details += "s${i}: window ($($w.x),$($w.y)) $($w.w)x$($w.h) not on $($screen.name)" }
    }
    if (-not $r.exitedByItself) { $ok = $false; $details += 'client did not quit after the host closed the app (error dialog?)' }
    Add-Check $case $ok ($details -join '; ')

    if ($r.mouse) { Add-Check "$case : the mouse reaches each display" $r.mouse.ok ($r.mouse.details -join '; ') }
}

# --- Results -----------------------------------------------------------------------------------------------------

function Write-Results {
    $client = Get-Item (Join-Path $ClientDir 'Moonlight.exe') -ErrorAction SilentlyContinue
    $lines = @('# Phase 3 results (Qt client, multi-display, dev PC)', '', "Run: $script:Started .. $(Get-Date -Format o)", "Machine: $env:COMPUTERNAME",
        "Elevated: $Elevated", "Client: $ClientDir (Moonlight.exe $(if ($client) { $client.LastWriteTime.ToString('s') }))",
        "Host: $((Get-Content (Join-Path $ProjectRoot 'stage\phase2\host-source.txt') -ErrorAction SilentlyContinue) -join ' ')",
        "Physical monitors: $(($script:Physical | ForEach-Object { '{0} {1}x{2}' -f $_.name, $_.w, $_.h }) -join ', ')", '',
        'Per stream, from the client''s "Tessera stats" lines (the first 10 s left out): frames received and rendered per second, decode and render time per frame, frames lost on the network and dropped by the pacer. Content: the frame rate ffplay kept up on the host''s display.', '',
        '| Case | Plan | Accepted | Reason | Stream | Video | Hz | Received fps | Rendered fps | Content fps | Decode ms | Render ms | Network dropped | Pacer dropped |',
        '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($case in $Cases) {
        $r = $script:Results[$case]
        if (-not $r) { continue }
        if ($r.error -or -not $r.log) { $lines += "| $case | error: $($r.error) |  |  |  |  |  |  |  |  |  |  |  |  |"; continue }
        $log = $r.log
        $accepted = if ($null -ne $log.accepted) { $log.accepted } else { 1 }
        $keys = @($log.stats.Keys | Sort-Object)
        if (-not $keys.Count) { $lines += "| $case | $($log.plan.Count) | $accepted | $($log.reason) | (no statistics) |  |  |  |  |  |  |  |  |  |" }
        foreach ($i in $keys) {
            $s = $log.stats[$i]
            $content = if ($i -lt $r.content.Count -and $null -ne $r.content[$i]) { $r.content[$i] } else { '' }
            $hz = if ($i -lt $r.rates.Count) { $r.rates[$i] } else { '' }
            $lines += '| {0} | {1} | {2} | {3} | {4} | {5}x{6} | {7} | {8:N1} | {9:N1} | {10} | {11:N2} | {12:N2} | {13} | {14} |' -f $case, $log.plan.Count, $accepted, $log.reason, $i,
                $s.w, $s.h, $hz, $s.receivedFps, $s.renderedFps, $content, $s.decodeMs, $s.renderMs, $s.netDropped, $s.pacerDropped
        }
    }
    $lines += @('', 'Checks:')
    foreach ($c in $script:Checks) { $lines += "- $c" }
    $errors = @()
    foreach ($case in $Cases) {
        $r = $script:Results[$case]
        if ($r -and $r.log -and $r.log.errors.Count) { $errors += "${case}:"; $errors += $r.log.errors | Select-Object -First 10 }
    }
    if ($errors.Count) { $lines += @('', 'Errors in the client logs (first 10 per case):', '', '```') + $errors + '```' }
    $capture = @(Get-ChildItem (Join-Path $OutDir 'cases') -Recurse -Filter 'host-log-excerpt.txt' -ErrorAction SilentlyContinue | ForEach-Object {
        $name = $_.Directory.Name
        Select-String -Path $_.FullName -SimpleMatch 'Tessera: capture' | ForEach-Object { "${name}: $($_.Line)" }
    })
    if ($capture.Count) { $lines += @('', 'Capture statistics (host, per display, every 20 s):', '', '```') + $capture + '```' }
    $lines | Set-Content -Encoding UTF8 (Join-Path $OutDir 'results.md')
}

# --- Run ---------------------------------------------------------------------------------------------------------

New-Item -ItemType Directory -Force $OutDir | Out-Null
Start-Transcript -Path (Join-Path $OutDir 'harness.log') | Out-Null
$script:Started = Get-Date -Format o
$script:Results = @{}
$script:Checks = @()
$script:Physical = @()
$script:TesseraHost = $null
$script:StockHost = $null
try {
    Set-Unattended
    $script:Physical = @(Get-Screens)  # before the driver is installed: the PC's own monitors, in pixels
    Log "physical monitors: $(($script:Physical | ForEach-Object { '{0} {1}x{2}' -f $_.name, $_.w, $_.h }) -join ', '); elevated: $Elevated"
    $Definitions = @{
        'single'         = @{ mode = 0; standIns = @() }
        'ms2'            = @{ mode = 1; standIns = (Get-StandIns 2 '1920x1080@60'); mouse = $true }
        'ms3'            = @{ mode = 1; standIns = (Get-StandIns 3 '1920x1080@60') }
        'ms4'            = @{ mode = 1; standIns = (Get-StandIns 4 '1920x1080@60') }
        'mixed'          = @{ mode = 1; standIns = @('2560x1440@60', '1280x720@60'); mouse = $true }
        'hz120'          = @{ mode = 1; standIns = @('1920x1080@120') }
        'hz144'          = @{ mode = 1; standIns = @('2560x1440@144') }
        'hz180'          = @{ mode = 1; standIns = @('1920x1080@180') }
        'borderless'     = @{ mode = 1; standIns = (Get-StandIns 2 '1920x1080@60'); display = 'borderless'; mouse = $true }
        'unplug'         = @{ mode = 1; standIns = @('1920x1080@60'); unplug = $true }
        'not-permitted'  = @{ mode = 1; standIns = (Get-StandIns 2 '1920x1080@60'); expect = 1; reason = 'not-permitted' }
        'limit'          = @{ mode = 1; standIns = (Get-StandIns 3 '1920x1080@60'); expect = 2; reason = 'host-limit' }
        'stock-sunshine' = @{ mode = 1; standIns = $(if ($Elevated) { Get-StandIns 2 '1920x1080@60' } else { @() }); expect = 1; hostName = 'stock' }
    }

    Log 'staging'
    Initialize-Stage
    if ($Elevated) {
        if ((Invoke-Tool (Join-Path $Stage 'topoctl.exe') @('save', (Quote (Join-Path $OutDir 'topology-before.json'))) (Join-Path $OutDir 'topology-save.log') 1) -ne 0) { throw 'could not save the topology' }
        $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('install', (Quote (Join-Path $Stage 'package\TesseraVdd.inf'))) (Join-Path $OutDir 'vdd-install.log') 5
        Log "vdd-install install: exit $rc"
    }

    $moonlight = Join-Path $Stage 'client\Moonlight.exe'
    $tesseraCases = @($Cases | Where-Object { $_ -ne 'stock-sunshine' })
    if ($tesseraCases.Count) {
        Log 'Tessera host'
        $script:TesseraHost = Start-HostInstance 'tessera' (Join-Path $Stage 'host') $HostOverrides
        $rc = Complete-Pairing $moonlight @('pair', '127.0.0.1', '--pin', $Pin) $ClientName (Join-Path $OutDir 'pair-tessera.log') (Join-Path $Stage 'state-tessera')
        Add-Check 'the Qt client pairs with the Tessera host' ($rc -eq 0) "exit $rc"

        foreach ($case in $tesseraCases) {
            $def = $Definitions[$case]
            if (-not $def) { Log "unknown case $case"; continue }
            if ($def.standIns.Count -and -not $Elevated) { Log "case $case skipped: needs elevation"; continue }
            if ($case -eq 'limit') {
                Stop-HostInstance $script:TesseraHost
                $script:TesseraHost = Start-HostInstance 'tessera' (Join-Path $Stage 'host') ($HostOverrides + 'tessera_max_streams=2')
            }
            $uuid = $null
            if ($case -eq 'not-permitted') {
                $uuid = @(((Invoke-WebApi GET '/api/clients/list' '') | ConvertFrom-Json).named_certs | Where-Object { $_.name -eq $ClientName })[0].uuid
                Log "  multi-display off for $ClientName : $(Invoke-WebApi POST '/api/tessera/clients/update' (@{ uuid = $uuid; multistream = $false } | ConvertTo-Json -Compress))"
            }
            Log "case $case"
            Invoke-QtCase $case $def
            if ($uuid) { Log "  multi-display on again: $(Invoke-WebApi POST '/api/tessera/clients/update' (@{ uuid = $uuid; multistream = $true } | ConvertTo-Json -Compress))" }
            if ($case -eq 'limit') {
                Stop-HostInstance $script:TesseraHost
                $script:TesseraHost = Start-HostInstance 'tessera' (Join-Path $Stage 'host') $HostOverrides
            }
        }
        Stop-HostInstance $script:TesseraHost
        $script:TesseraHost = $null
        if ($Elevated) { Test-Topology 'after-tessera' | Out-Null }
    }

    if ($Cases -contains 'stock-sunshine') {
        Log 'case stock-sunshine'
        $script:StockHost = Start-HostInstance 'stock' (Join-Path $Stage 'sunshine-stock')
        $rc = Complete-Pairing $moonlight @('pair', '127.0.0.1', '--pin', $Pin) $ClientName (Join-Path $OutDir 'pair-stock.log') (Join-Path $Stage 'state-stock')
        Add-Check 'the Qt client pairs with stock Sunshine' ($rc -eq 0) "exit $rc"
        Invoke-QtCase 'stock-sunshine' $Definitions['stock-sunshine']
        Stop-HostInstance $script:StockHost
        $script:StockHost = $null
    }

    foreach ($case in $Cases) { Test-QtCase $case }
} catch {
    Log "ERROR: $_"
    $script:Checks += "FAIL harness: $_"
} finally {
    foreach ($h in $script:TesseraHost, $script:StockHost) { if ($h) { Stop-HostInstance $h } }
    # Only what this run started (everything it runs is in the stage)
    Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($Stage, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($Elevated) {
        Log 'cleaning up'
        Invoke-Tool (Join-Path $Stage 'recover.exe') @('--restore', '--fallback') (Join-Path $OutDir 'recover-end.log') 5 | Out-Null
        $rc = Invoke-Tool (Join-Path $Stage 'vdd-install.exe') @('remove', '--purge') (Join-Path $OutDir 'vdd-uninstall.log') 30
        Log "vdd-install remove --purge: exit $rc"
        $script:Checks += "INFO topology at the end: $(if (Test-Topology 'end') { 'as before' } else { 'different' })"
    }
    Write-Results
    Log "done; results in $(Join-Path $OutDir 'results.md')"
    'done' | Set-Content (Join-Path $OutDir 'DONE')
    Stop-Transcript | Out-Null
}
