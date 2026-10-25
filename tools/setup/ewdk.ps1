<#
.SYNOPSIS
  Runs a command inside the Enterprise WDK build environment (MSVC 14.50, Windows SDK and WDK 10.0.28000).

.DESCRIPTION
  Mounts the EWDK ISO if it is not mounted yet (no admin rights needed for ISO files), then runs the command
  in cmd.exe after SetupBuildEnv.cmd. See docs/adr/0004-dev-environment.md.

.EXAMPLE
  .\tools\setup\ewdk.ps1 'msbuild drivers\vdd\TesseraVdd.vcxproj /p:Configuration=Release /p:Platform=x64'
#>
param(
    [Parameter(Mandatory, Position = 0)] [string]$Command,
    [string]$Iso = 'D:\Remote streaming\toolchain\dl\EWDK_28000.iso',
    [string]$Arch = 'amd64'
)
$ErrorActionPreference = 'Stop'
$img = Get-DiskImage -ImagePath $Iso
if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $Iso -PassThru }
$letter = ($img | Get-Volume).DriveLetter
if (-not $letter) { throw "EWDK ISO mounted but has no drive letter" }
$setup = "${letter}:\BuildEnv\SetupBuildEnv.cmd"
if (-not (Test-Path $setup)) { throw "not an EWDK image: $setup missing" }
# SetupBuildEnv puts MSBuild and the compiler on PATH, but not the WDK tools (signtool, Inf2Cat, InfVerif).
$kit = "${letter}:\Program Files\Windows Kits\10"
$ver = (Get-ChildItem "$kit\bin" -Directory | Where-Object Name -like '10.0.*' | Sort-Object Name | Select-Object -Last 1).Name
$env:PATH = "$kit\bin\$ver\x64;$kit\bin\$ver\x86;$kit\Tools\$ver\x64;$env:PATH"
# SetupBuildEnv prints a harmless "vswhere.exe is not recognized" on stderr; Windows PowerShell would turn
# native stderr into a terminating error under 'Stop', so silence the setup and relax the preference.
$ErrorActionPreference = 'Continue'
cmd.exe /d /s /c "`"call `"$setup`" $Arch >nul 2>&1 && $Command`""
exit $LASTEXITCODE
