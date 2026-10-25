<#
.SYNOPSIS
  Development environment for Tessera on the dev PC (see docs/adr/0004-dev-environment.md).
  Dot-source it:  . 'D:\Remote streaming\src\tools\setup\env.ps1'

.DESCRIPTION
  - Maps T: to the project folder (subst), because MSYS2 and the Android NDK cannot handle the space in
    "D:\Remote streaming". subst is per logon session, so this is re-run in every new session.
  - Points package-manager caches at T:\cache, because C: is nearly full.
  - Puts the portable tools (gh) on PATH.
  Skips the subst when -Root has no space or is on NTFS and the caller passes -NoSubst.
#>
param(
    [string]$Root = 'D:\Remote streaming',
    [string]$Drive = 'T:',
    [switch]$NoSubst
)

if (-not $NoSubst -and -not (Test-Path "$Drive\")) {
    subst $Drive "$Root"
    if (-not (Test-Path "$Drive\")) { throw "subst $Drive `"$Root`" failed" }
}
$base = if ($NoSubst) { $Root } else { "$Drive" }

$env:TESSERA_ROOT = "$base\"
$cache = Join-Path $base 'cache'
$env:npm_config_cache = Join-Path $cache 'npm'
$env:GRADLE_USER_HOME = Join-Path $cache 'gradle'
$env:PIP_CACHE_DIR = Join-Path $cache 'pip'
$env:UV_CACHE_DIR = Join-Path $cache 'uv'
$env:NUGET_PACKAGES = Join-Path $cache 'nuget'

$gh = Join-Path $base 'toolchain\gh\bin'
if ((Test-Path $gh) -and ($env:PATH -notlike "*$gh*")) { $env:PATH = "$gh;$env:PATH" }

Write-Host "Tessera environment: repo $base\src, toolchain $base\toolchain, caches $cache"
