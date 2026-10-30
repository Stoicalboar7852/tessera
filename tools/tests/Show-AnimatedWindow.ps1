<#
.SYNOPSIS
  A window that changes every frame, labelled with its display, for the Phase 2 multistream tests.
.DESCRIPTION
  Covers the given rectangle (desktop coordinates) with a moving bar, a frame counter and a large label, so every
  stream has new frames to encode and its snapshot shows which display it came from. Runs until closed or killed.
#>
param(
    [int]$X,
    [int]$Y,
    [int]$Width,
    [int]$Height,
    [string]$Label,
    [switch]$Static  # no animation (the "typical" case's quiet displays)
)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point($X, $Y)
$form.Size = New-Object System.Drawing.Size($Width, $Height)
$form.TopMost = $true
$form.ShowInTaskbar = $false
$form.BackColor = [System.Drawing.Color]::FromArgb(24, 24, 32)
$doubleBuffered = [System.Windows.Forms.Control].GetProperty('DoubleBuffered', [System.Reflection.BindingFlags]'Instance, NonPublic')
$doubleBuffered.SetValue($form, $true, $null)

$font = New-Object System.Drawing.Font('Segoe UI', [float]([Math]::Max(24, $Height / 8)), [System.Drawing.FontStyle]::Bold)
$small = New-Object System.Drawing.Font('Consolas', 20)
$script:frame = 0
$form.Add_Paint({
    param($s, $e)
    $g = $e.Graphics
    $g.DrawString($Label, $font, [System.Drawing.Brushes]::White, 40, 40)
    $g.DrawString(("frame {0}" -f $script:frame), $small, [System.Drawing.Brushes]::LightGray, 44, $Height - 80)
    $barWidth = [Math]::Max(40, $Width / 10)
    $x = ($script:frame * 12) % ($Width + $barWidth) - $barWidth
    $g.FillRectangle([System.Drawing.Brushes]::OrangeRed, $x, $Height / 2, $barWidth, $Height / 6)
})

if (-not $Static) {
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 10
    $timer.Add_Tick({ $script:frame++; $form.Invalidate() })
    $timer.Start()
}
[System.Windows.Forms.Application]::Run($form)
