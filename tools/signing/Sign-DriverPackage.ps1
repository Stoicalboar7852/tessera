<#
.SYNOPSIS
  Assembles, catalogs and signs the Tessera VDD driver package with the local development certificate.

.DESCRIPTION
  Development signing only (no test-signing mode, so Secure Boot can stay on): the certificate is a
  self-signed code-signing certificate whose public part the developer trusted once with
  tools/setup/admin-once.ps1. Release signing is documented in docs/release-signing.md.
  Runs inside the EWDK environment (Inf2Cat, InfVerif and signtool come from the WDK).

.EXAMPLE
  .\tools\signing\Sign-DriverPackage.ps1 -BuildDir T:\build\vdd\x64\Release -Thumbprint 1265345C...
#>
param(
    [Parameter(Mandatory)] [string]$BuildDir,
    [string]$Thumbprint = '1265345CE0C4B20790063F3999873002D93FAD2A',
    [string]$OutDir = (Join-Path $BuildDir 'package')
)
$ErrorActionPreference = 'Stop'
$cert = Get-Item "Cert:\CurrentUser\My\$Thumbprint" -ErrorAction SilentlyContinue
if (-not $cert) { throw "signing certificate $Thumbprint not found in CurrentUser\My" }

New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($f in 'TesseraVdd.dll', 'TesseraVdd.inf') {
    $src = Join-Path $BuildDir $f
    if (-not (Test-Path $src)) { throw "missing $src (build the driver first)" }
    Copy-Item $src $OutDir -Force
}
Copy-Item $cert.PSPath -Destination $OutDir -ErrorAction SilentlyContinue
Export-Certificate -Cert $cert -FilePath (Join-Path $OutDir 'tessera-dev-signing.cer') | Out-Null

$ewdk = Join-Path $PSScriptRoot '..\setup\ewdk.ps1'
$inf = Join-Path $OutDir 'TesseraVdd.inf'
# stampinf writes the local date; Inf2Cat rejects a DriverVer date that is in the future in UTC.
$utc = (Get-Date).ToUniversalTime().ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
$text = (Get-Content -Raw $inf) -replace '(?m)^DriverVer\s*=\s*[0-9/]+,', "DriverVer = $utc,"
[IO.File]::WriteAllText($inf, $text)
$dll = Join-Path $OutDir 'TesseraVdd.dll'
$cat = Join-Path $OutDir 'TesseraVdd.cat'
$steps = @(
    "infverif /w /v `"$inf`"",
    "signtool sign /q /fd sha256 /sha1 $Thumbprint /s My `"$dll`"",
    "inf2cat /driver:`"$OutDir`" /os:10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64",
    "signtool sign /q /fd sha256 /sha1 $Thumbprint /s My `"$cat`"",
    "signtool verify /pa /q `"$dll`"",
    "signtool verify /pa /q `"$cat`""
)
foreach ($s in $steps) {
    Write-Host "> $s"
    & $ewdk $s
    if ($LASTEXITCODE -ne 0) { throw "step failed ($LASTEXITCODE): $s" }
}
Write-Host "Signed package: $OutDir"
