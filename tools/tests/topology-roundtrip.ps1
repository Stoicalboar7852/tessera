<#
.SYNOPSIS
  Phase 0 acceptance test: recover.exe restores a deliberately changed display topology.

.DESCRIPTION
  For each scenario: save the current layout, make a journaled change with topoctl (the way the host will),
  check that the layout really changed, run `recover --restore`, and check that the layout is back exactly.
  CHANGES THE REAL DISPLAY LAYOUT for a few seconds per scenario. Every change is journaled, and a final
  `recover --restore --fallback` runs even if a scenario fails.

.EXAMPLE
  .\tools\tests\topology-roundtrip.ps1 -BinDir T:\build\native-mingw\tools\recover
#>
param(
    [Parameter(Mandatory)] [string]$BinDir,
    [string[]]$Scenarios = @('move-secondary', 'swap-primary', 'refresh', 'disable-secondary'),
    [string]$Journal
)

$ErrorActionPreference = 'Stop'
$topoctl = Join-Path $BinDir 'topoctl.exe'
$recover = Join-Path $BinDir 'recover.exe'
foreach ($exe in $topoctl, $recover) { if (-not (Test-Path $exe)) { throw "missing $exe" } }
$journalArgs = if ($Journal) { @('--journal', $Journal) } else { @() }
$work = New-Item -ItemType Directory -Force (Join-Path $env:TEMP ("tessera-topotest-" + (Get-Date -Format yyyyMMdd-HHmmss)))
$log = Join-Path $work 'run.log'

function Run([string]$exe, [string[]]$argv) {
    $out = & $exe @argv 2>&1 | Out-String
    Add-Content -Path $log -Value ("> " + (Split-Path $exe -Leaf) + " " + ($argv -join ' ') + "`n" + $out + "exit=$LASTEXITCODE`n")
    return [pscustomobject]@{ Exit = $LASTEXITCODE; Out = $out.Trim() }
}

$pre = Run $recover (@('--list') + $journalArgs)
if ($pre.Out -notmatch ', 0 entries') {
    throw "journal is not empty before the test; run recover --restore first:`n$($pre.Out)"
}

$results = @()
try {
    foreach ($s in $Scenarios) {
        $before = Join-Path $work "$s-before.json"
        $save = Run $topoctl @('save', $before)
        if ($save.Exit -ne 0) { throw "topoctl save failed: $($save.Out)" }

        $change = Run $topoctl (@('change', $s, '--owner', "topology-roundtrip:$s") + $journalArgs)
        if ($change.Exit -ne 0) {
            $results += [pscustomobject]@{ Scenario = $s; Result = 'SKIPPED'; Seconds = ''; Detail = ($change.Out -split "`n")[0] }
            continue
        }
        Start-Sleep -Seconds 2
        $changed = (Run $topoctl @('compare', $before)).Exit -ne 0

        $sw = [Diagnostics.Stopwatch]::StartNew()
        $restore = Run $recover (@('--restore') + $journalArgs)
        $sw.Stop()
        $after = Run $topoctl @('compare', $before)

        $ok = $changed -and $restore.Exit -eq 0 -and $after.Exit -eq 0
        $detail = if (-not $changed) { 'layout did not change' } elseif ($restore.Exit -ne 0) { "recover exit $($restore.Exit)" } elseif ($after.Exit -ne 0) { $after.Out -replace "`r?`n", ' ' } else { 'restored exactly' }
        $results += [pscustomobject]@{ Scenario = $s; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Detail = $detail }
    }
}
finally {
    $final = Run $recover (@('--restore', '--fallback') + $journalArgs)
    Add-Content -Path $log -Value "final safety restore exit=$($final.Exit)"
}

$results | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "log: $log"
$failed = @($results | Where-Object Result -eq 'FAIL').Count
$passed = @($results | Where-Object Result -eq 'PASS').Count
if ($passed -eq 0) { Write-Host 'No scenario could run.'; exit 2 }
exit $failed
