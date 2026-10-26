<#
.SYNOPSIS
  Phase 0 check: the dev-signed Tessera VDD installs and loads in the test VM with Secure Boot ON.

.DESCRIPTION
  Over PowerShell Direct (no network needed):
    1. confirms Secure Boot is on in the guest;
    2. tries to install the driver WITHOUT trusting the dev certificate (must be refused);
    3. trusts the certificate, installs again, and checks the device is started, the driver DLL is loaded
       in a WUDFHost process, and the control interface answers;
    4. removes the device and the driver package again.
  The guest credentials come from the git-ignored <VmRoot>\local\credentials.json written by New-TestVm.ps1.
#>
param(
    [string]$Name = 'tessera-test',
    [string]$VmRoot = 'D:\Remote streaming\vm',
    [Parameter(Mandatory)] [string]$PackageDir,
    [Parameter(Mandatory)] [string]$VddInstallExe,
    [int]$WaitMinutes = 60
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VmCommon.ps1')
$c = Get-Content -Raw (Join-Path $VmRoot 'local\credentials.json') | ConvertFrom-Json
$cred = New-Object PSCredential($c.user, (ConvertTo-SecureString $c.password -AsPlainText -Force))

# Wait until the guest accepts PowerShell Direct (Windows setup and first logon finished).
$deadline = (Get-Date).AddMinutes($WaitMinutes)
$session = $null
while (-not $session) {
    try { $session = New-PSSession -VMName $Name -Credential $cred -ErrorAction Stop }
    catch {
        if ((Get-Date) -gt $deadline) { throw "guest not reachable over PowerShell Direct after $WaitMinutes min: $($_.Exception.Message)" }
        Start-Sleep -Seconds 30
    }
}
try {
    Invoke-Command -Session $session { New-Item -ItemType Directory -Force C:\tessera | Out-Null }
    foreach ($f in Get-ChildItem $PackageDir -File) { Send-VmFile $session $f.FullName "C:\tessera\$($f.Name)" }
    Send-VmFile $session $VddInstallExe 'C:\tessera\vdd-install.exe'

    $result = Invoke-Command -Session $session {
        $r = [ordered]@{}
        $r.os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
        $r.secure_boot = Confirm-SecureBootUEFI
        $r.hvci = ((Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard).SecurityServicesRunning -contains 2)
        $r.test_signing = [bool]((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions -match 'TESTSIGNING')

        $cer = Get-ChildItem C:\tessera\*.cer | Select-Object -First 1
        $thumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2($cer.FullName)).Thumbprint
        # certutil, not the PKI cmdlets: Import-Certificate fails with access denied in a PowerShell Direct session.
        foreach ($s in 'Root', 'TrustedPublisher') { certutil -delstore $s $thumb 2>&1 | Out-Null }
        $r.cert_trusted_before = [bool](Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue | Where-Object Thumbprint -eq $thumb)

        # Runs vdd-install with a time limit: an untrusted publisher can trigger a consent prompt that never
        # gets answered in this non-interactive session.
        function Invoke-VddInstall([string[]]$argv, [int]$timeoutSec = 180) {
            $log = "C:\tessera\vdd-install-$([guid]::NewGuid().ToString('N')).log"
            $p = Start-Process C:\tessera\vdd-install.exe -ArgumentList $argv -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError "$log.err"
            $null = $p.Handle  # keep the handle open, or ExitCode is empty after the process exits
            if (-not $p.WaitForExit($timeoutSec * 1000)) {
                $p.Kill()
                return [pscustomobject]@{ Exit = 'timeout'; Out = "no result within $timeoutSec s (waiting for a consent prompt?)" }
            }
            $text = ((Get-Content $log -Raw -ErrorAction SilentlyContinue) + (Get-Content "$log.err" -Raw -ErrorAction SilentlyContinue)).Trim()
            [pscustomobject]@{ Exit = $p.ExitCode; Out = $text }
        }

        # Start clean (an earlier, interrupted run may have left a device or package behind).
        Invoke-VddInstall @('remove', '--purge') | Out-Null

        # 1) Untrusted certificate: installation must not succeed.
        $u = Invoke-VddInstall @('install', 'C:\tessera\TesseraVdd.inf')
        $r.untrusted_install_exit = $u.Exit
        $r.untrusted_install_output = $u.Out
        Invoke-VddInstall @('remove') | Out-Null

        # 2) Trusted certificate: installation must succeed and the driver must run.
        foreach ($s in 'Root', 'TrustedPublisher') { certutil -f -addstore $s $cer.FullName 2>&1 | Out-Null }
        $t = Invoke-VddInstall @('install', 'C:\tessera\TesseraVdd.inf')
        $r.trusted_install_exit = $t.Exit
        $r.trusted_install_output = $t.Out
        Start-Sleep -Seconds 5
        $r.status = (& C:\tessera\vdd-install.exe status 2>&1 | Out-String).Trim()
        $dev = Get-PnpDevice -PresentOnly | Where-Object HardwareID -contains 'Root\TesseraVdd'
        $r.pnp_status = if ($dev) { "$($dev.Status) problem=$($dev.Problem)" } else { 'not present' }
        $r.query = (& C:\tessera\vdd-install.exe query 2>&1 | Out-String).Trim()
        $r.wudfhost_loaded = [bool](Get-Process WUDFHost -ErrorAction SilentlyContinue | Where-Object { try { $_.Modules.ModuleName -contains 'TesseraVdd.dll' } catch { $false } })
        $r.code_integrity_blocks = @(Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 50 -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -in 3033, 3034, 3077, 3089 -and $_.Message -match 'Tessera' } | ForEach-Object { "$($_.Id): $(($_.Message -split "`n")[0])" })

        # 3) Clean up.
        $r.remove = (& C:\tessera\vdd-install.exe remove --purge 2>&1 | Out-String).Trim()
        [pscustomobject]$r
    }
    $result | Format-List | Out-String -Width 200 | Write-Host
    $ok = $result.secure_boot -and -not $result.test_signing -and -not $result.cert_trusted_before -and "$($result.untrusted_install_exit)" -ne '0' -and
          "$($result.trusted_install_exit)" -eq '0' -and $result.pnp_status -like 'OK*' -and $result.query -like 'Tessera VDD*'
    Write-Host ($(if ($ok) { 'PASS: dev-signed VDD installs and runs with Secure Boot on; untrusted signature refused' } else { 'FAIL' }))
    exit $(if ($ok) { 0 } else { 1 })
}
finally {
    if ($session) { Remove-PSSession $session }
}
