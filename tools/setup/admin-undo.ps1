#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Reverses admin-once.ps1: removes the dev certificate(s) from the machine stores, removes the user from
  Hyper-V Administrators, and (only with -DisableHyperV) disables the Hyper-V feature.
  Without -Thumbprint, every "Tessera Dev Driver Signing" certificate in the two machine stores is removed.
#>
param(
    [string]$User = "$env:COMPUTERNAME\$env:USERNAME",
    [string]$Thumbprint,
    [switch]$DisableHyperV
)

$ErrorActionPreference = 'Continue'
foreach ($store in 'Root', 'TrustedPublisher') {
    $certs = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object {
        if ($Thumbprint) { $_.Thumbprint -eq $Thumbprint } else { $_.Subject -like 'CN=Tessera Dev Driver Signing*' }
    }
    foreach ($c in $certs) { Remove-Item $c.PSPath; Write-Host "removed $($c.Thumbprint) from LocalMachine\$store" }
}
$group = Get-LocalGroup -SID 'S-1-5-32-578'
if (Get-LocalGroupMember -Group $group | Where-Object { $_.Name -eq $User }) {
    Remove-LocalGroupMember -Group $group -Member $User; Write-Host "removed $User from $($group.Name)"
}
if ($DisableHyperV) {
    Disable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -NoRestart | Out-Null
    Write-Host 'Hyper-V disabled (after reboot)'
}
