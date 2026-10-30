<#
.SYNOPSIS
  Fills <project>\stage\phase2 for Run-MultiStreamTest.ps1 (not elevated): the Tessera host build from CI, stock
  Moonlight, stock Sunshine and FFmpeg from their GitHub releases. Needs gh (signed in) on PATH.
.PARAMETER RunId
  The host CI run to take the build from; default: the latest successful "host (Windows)" run on main.
#>
param(
    [string]$ProjectRoot,
    [string]$RunId
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
$base = Join-Path $ProjectRoot 'stage\phase2'
New-Item -ItemType Directory -Force $base | Out-Null

# The Tessera host
if (-not $RunId) {
    $RunId = (gh run list --repo Stoicalboar7852/tessera --workflow ci-host-windows.yml --branch main --status success --limit 1 --json databaseId | ConvertFrom-Json)[0].databaseId
}
$hostDir = Join-Path $base 'host'
if (Test-Path $hostDir) { Remove-Item -Recurse -Force $hostDir }
gh run download $RunId --repo Stoicalboar7852/tessera --name host-windows-amd64 --dir $hostDir
# The artifact keeps host/build/ as its root
$built = Get-ChildItem $hostDir -Recurse -Filter sunshine.exe | Select-Object -First 1
if (-not $built) { throw "no sunshine.exe in run $RunId's artifact" }
if ($built.Directory.FullName -ne $hostDir) {
    Get-ChildItem $built.Directory.FullName | Move-Item -Destination $hostDir
}
if (-not (Test-Path (Join-Path $hostDir 'assets'))) { throw "run $RunId's artifact has no assets folder" }
"host: run $RunId" | Set-Content (Join-Path $base 'host-source.txt')

# Stock releases, each in its own folder
$releases = @(
    @{ dir = 'moonlight'; repo = 'moonlight-stream/moonlight-qt'; pattern = 'MoonlightPortable-x64-*.zip' },
    @{ dir = 'sunshine-stock'; repo = 'LizardByte/Sunshine'; pattern = 'Sunshine-Windows-AMD64-lite.zip' },
    @{ dir = 'ffmpeg'; repo = 'BtbN/FFmpeg-Builds'; pattern = 'ffmpeg-master-latest-win64-lgpl-shared.zip' }
)
foreach ($r in $releases) {
    $dir = Join-Path $base $r.dir
    if (Test-Path (Join-Path $dir 'x')) { continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    gh release download --repo $r.repo --pattern $r.pattern --dir $dir --clobber
    $zip = Get-ChildItem $dir -Filter *.zip | Select-Object -First 1
    Expand-Archive -Force $zip.FullName -DestinationPath (Join-Path $dir 'x')
}
Write-Host "stage ready: $base"
