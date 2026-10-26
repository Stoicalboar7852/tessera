#Requires -RunAsAdministrator
<#
.SYNOPSIS
  One-time elevated setup for the development PC (and again after every Windows reinstall).

.DESCRIPTION
  Does exactly three things and logs them to <project>\logs\admin-once.log:
    1. Enables the Hyper-V feature (takes effect after the next reboot; no reboot is forced).
    2. Adds the developer account to the local "Hyper-V Administrators" group, so test VMs can be
       created and driven without elevation (takes effect at next logon).
    3. Trusts the local development code-signing certificate (public part only) in
       LocalMachine\Root and LocalMachine\TrustedPublisher, so locally signed test drivers install.
       The private key stays non-exportable in the developer's CurrentUser\My store.
  Without -CertPath, the newest "Tessera Dev Driver Signing" certificate in the caller's CurrentUser\My
  is used, so run it elevated from the developer's own account. Undo with admin-undo.ps1.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\setup\admin-once.ps1"
#>
param(
    [string]$User = "$env:COMPUTERNAME\$env:USERNAME",
    [string]$CertPath,
    [string]$ExpectedThumbprint,
    [string]$LogDir = (Join-Path $PSScriptRoot '..\..\..\logs')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $LogDir | Out-Null
Start-Transcript -Path (Join-Path $LogDir 'admin-once.log') -Append | Out-Null
$failed = $false

function Step([string]$name, [scriptblock]$body) {
    Write-Host "==> $name"
    try { & $body; Write-Host "    OK" }
    catch { $script:failed = $true; Write-Host "    FAILED: $($_.Exception.Message)" -ForegroundColor Red }
}

Step 'Enable Hyper-V (no restart)' {
    $f = Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All
    if ($f.State -eq 'Enabled') { Write-Host '    already enabled' }
    else {
        $r = Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -All -NoRestart
        Write-Host "    RestartNeeded=$($r.RestartNeeded)"
    }
}

Step "Add $User to Hyper-V Administrators" {
    # S-1-5-32-578 is the built-in Hyper-V Administrators group (name is localised).
    $group = Get-LocalGroup -SID 'S-1-5-32-578'
    $already = Get-LocalGroupMember -Group $group | Where-Object { $_.Name -eq $User }
    if ($already) { Write-Host '    already a member' } else { Add-LocalGroupMember -Group $group -Member $User }
}

Step 'Trust development code-signing certificate' {
    if ($CertPath) {
        if (-not $ExpectedThumbprint) { throw '-CertPath needs -ExpectedThumbprint.' }
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($CertPath)
        if ($cert.Thumbprint -ne $ExpectedThumbprint) {
            throw "Thumbprint mismatch: file has $($cert.Thumbprint), expected $ExpectedThumbprint. Not importing."
        }
    } else {
        $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
            Where-Object Subject -like 'CN=Tessera Dev Driver Signing*' | Sort-Object NotBefore -Descending | Select-Object -First 1
        if (-not $cert) { throw 'No "Tessera Dev Driver Signing" certificate in CurrentUser\My (see README step 5).' }
        Write-Host "    certificate $($cert.Thumbprint) from CurrentUser\My"
    }
    $eku = $cert.Extensions | Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] }
    if (-not $eku -or $eku.EnhancedKeyUsages.Count -ne 1 -or $eku.EnhancedKeyUsages[0].Value -ne '1.3.6.1.5.5.7.3.3') {
        throw 'Certificate is not restricted to Code Signing only. Not importing.'
    }
    # Import the public part only (never the private key).
    $public = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(, $cert.RawData)
    foreach ($name in 'Root', 'TrustedPublisher') {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, 'LocalMachine')
        $store.Open('ReadWrite')
        try { $store.Add($public) } finally { $store.Close() }
        Write-Host "    imported into LocalMachine\$name"
    }
}

if ($failed) { Write-Host "`nSome steps FAILED - see above." -ForegroundColor Red }
else { Write-Host "`nAll steps done. Reboot when convenient to finish enabling Hyper-V." -ForegroundColor Green }
Stop-Transcript | Out-Null
