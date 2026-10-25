<#
.SYNOPSIS
  Saves a PNG of the test VM's screen (Hyper-V thumbnail API), to follow an unattended install.
#>
param(
    [string]$Name = 'tessera-test',
    [string]$Path = (Join-Path $env:TEMP 'tessera-vm.png'),
    [int]$Width = 1024,
    [int]$Height = 768
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$ns = 'root\virtualization\v2'
$vm = Get-CimInstance -Namespace $ns -ClassName Msvm_ComputerSystem -Filter "ElementName='$Name'"
$settings = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_VirtualSystemSettingData |
    Where-Object VirtualSystemType -eq 'Microsoft:Hyper-V:System:Realized'
$svc = Get-CimInstance -Namespace $ns -ClassName Msvm_VirtualSystemManagementService
$r = Invoke-CimMethod -InputObject $svc -MethodName GetVirtualSystemThumbnailImage -Arguments @{
    TargetSystem = $settings; WidthPixels = [uint16]$Width; HeightPixels = [uint16]$Height }
if ($r.ReturnValue -ne 0) { throw "GetVirtualSystemThumbnailImage returned $($r.ReturnValue)" }
$bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format16bppRgb565)
$rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
$pixels = [byte[]]$r.ImageData
$count = [Math]::Min($pixels.Length, $data.Stride * $Height)  # never write past the bitmap buffer
[Runtime.InteropServices.Marshal]::Copy($pixels, 0, $data.Scan0, $count)
$bmp.UnlockBits($data)
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
$Path
