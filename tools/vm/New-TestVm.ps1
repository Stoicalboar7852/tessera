<#
.SYNOPSIS
  Creates the Tessera test VM (Hyper-V Gen 2, Secure Boot + vTPM on) and starts an unattended Windows install.

.DESCRIPTION
  Needs membership in "Hyper-V Administrators" (tools/setup/admin-once.ps1), no elevation.
  Secrets (the VM admin password) go to <VmRoot>\local, outside the repository.
  Afterwards use Invoke-TestVm.ps1 to run commands in the guest over PowerShell Direct.
#>
param(
    [string]$Name = 'tessera-test',
    [string]$Iso = 'D:\Remote streaming\toolchain\dl\Win11_x64_en-us.iso',
    [string]$VmRoot = 'D:\Remote streaming\vm',
    [int64]$MemoryBytes = 6GB,
    [int]$Cpus = 4,
    [int64]$DiskBytes = 80GB
)
$ErrorActionPreference = 'Stop'
if (Get-VM -Name $Name -ErrorAction SilentlyContinue) { throw "VM '$Name' already exists" }

$local = Join-Path $VmRoot 'local'
$unattendDir = Join-Path $local 'unattend'
New-Item -ItemType Directory -Force $unattendDir | Out-Null

# Random password meeting complexity rules; stored only locally.
$chars = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'.ToCharArray()
$bytes = New-Object byte[] 24
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$password = 'Tz9-' + (-join ($bytes | ForEach-Object { $chars[$_ % $chars.Length] }))
@{ user = 'tessera'; password = $password; vm = $Name } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $local 'credentials.json')

$template = Get-Content -Raw (Join-Path $PSScriptRoot 'autounattend.template.xml')
[IO.File]::WriteAllText((Join-Path $unattendDir 'autounattend.xml'), $template.Replace('__PASSWORD__', [Security.SecurityElement]::Escape($password)), (New-Object Text.UTF8Encoding $false))

# Build a small ISO holding autounattend.xml (Windows Setup searches all drives for it) with IMAPI2.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices.ComTypes;
public static class TesseraIsoWriter {
    public static void Write(string path, object comStream) {
        IStream s = (IStream)comStream;
        byte[] buf = new byte[65536];
        IntPtr readPtr = System.Runtime.InteropServices.Marshal.AllocHGlobal(4);
        try {
            using (FileStream fs = File.Create(path)) {
                while (true) {
                    s.Read(buf, buf.Length, readPtr);
                    int n = System.Runtime.InteropServices.Marshal.ReadInt32(readPtr);
                    if (n <= 0) break;
                    fs.Write(buf, 0, n);
                }
            }
        } finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(readPtr); }
    }
}
'@
$fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$fsi.FileSystemsToCreate = 3  # ISO9660 + Joliet
$fsi.VolumeName = 'UNATTEND'
$fsi.Root.AddTree($unattendDir, $false)
$unattendIso = Join-Path $local 'autounattend.iso'
[TesseraIsoWriter]::Write($unattendIso, $fsi.CreateResultImage().ImageStream)

$vmDir = Join-Path $VmRoot $Name
$vhd = Join-Path $vmDir 'disk.vhdx'
New-Item -ItemType Directory -Force $vmDir | Out-Null
$vm = New-VM -Name $Name -Generation 2 -MemoryStartupBytes $MemoryBytes -Path $VmRoot -NewVHDPath $vhd -NewVHDSizeBytes $DiskBytes -SwitchName 'Default Switch'
Set-VMProcessor -VM $vm -Count $Cpus
Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
Set-VMFirmware -VM $vm -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows
Set-VMKeyProtector -VM $vm -NewLocalKeyProtector
Enable-VMTPM -VM $vm
Enable-VMIntegrationService -VM $vm -Name 'Guest Service Interface'
Set-VM -VM $vm -CheckpointType Disabled -AutomaticCheckpointsEnabled $false
$dvd = Add-VMDvdDrive -VM $vm -Path $Iso -Passthru
Add-VMDvdDrive -VM $vm -Path $unattendIso
Set-VMFirmware -VM $vm -FirstBootDevice $dvd

Start-VM -VM $vm
# The Windows ISO waits for "Press any key to boot from CD or DVD"; answer it through the VM's keyboard.
$cs = Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_ComputerSystem -Filter "ElementName='$Name'"
$kbd = Get-CimAssociatedInstance -InputObject $cs -ResultClassName Msvm_Keyboard
for ($i = 0; $i -lt 30; $i++) {
    Invoke-CimMethod -InputObject $kbd -MethodName TypeKey -Arguments @{ keyCode = 13 } | Out-Null
    Start-Sleep -Milliseconds 500
}
Write-Host "VM '$Name' started; unattended Windows setup is running. Credentials: $local\credentials.json"
