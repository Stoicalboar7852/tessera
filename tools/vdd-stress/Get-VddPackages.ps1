<#
.SYNOPSIS
  Downloads and unpacks the third-party VDD packages listed in third-party-vdds.json (no elevation needed).

.DESCRIPTION
  Each package goes to <project>\cache\vdd-repro\<name>\dl, is checked against its pinned SHA-256, and is unpacked
  with the portable 7-Zip in <project>\toolchain\7zip (NSIS installers) or Expand-Archive (zip). Every driver file
  is then checked against the manifest. Nothing is installed or executed.

.EXAMPLE
  .\tools\vdd-stress\Get-VddPackages.ps1
#>
param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$manifest = Get-Content -Raw (Join-Path $PSScriptRoot 'third-party-vdds.json') | ConvertFrom-Json
$cache = Join-Path $ProjectRoot 'cache\vdd-repro'
$sevenZip = Join-Path $ProjectRoot 'toolchain\7zip\7z.exe'

foreach ($d in $manifest.drivers) {
    $first = ($d.dir -split '\\')[0]
    $dl = Join-Path $cache "$first\dl"
    New-Item -ItemType Directory -Force $dl | Out-Null
    $file = Join-Path $dl ([IO.Path]::GetFileName($d.source))
    if (-not (Test-Path $file)) {
        Write-Host "downloading $($d.source)"
        Invoke-WebRequest $d.source -OutFile $file
    }
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $d.source_sha256) { throw "$($d.name): $file has SHA-256 $hash, expected $($d.source_sha256)" }

    $x = Join-Path $cache "$first\x"
    if ($file -like '*.zip') {
        Expand-Archive $file -DestinationPath $x -Force
    } else {
        if (-not (Test-Path $sevenZip)) { throw "7-Zip not found at $sevenZip (see tools/setup/README.md)" }
        # The NSIS uninstaller stub may fail to unpack; the driver files are checked below.
        & $sevenZip x -y -bso0 -bsp0 "-o$x" $file | Out-Null
    }
    foreach ($p in $d.files.PSObject.Properties) {
        $f = Join-Path (Join-Path $cache $d.dir) $p.Name
        if (-not (Test-Path $f)) { throw "$($d.name): missing $f after unpacking" }
        $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
        if ($h -ne $p.Value) { throw "$($d.name): $f has SHA-256 $h, expected $($p.Value)" }
    }
    Write-Host "$($d.name): OK ($($d.title))"
}
