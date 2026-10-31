<#
.SYNOPSIS
  A window that changes every frame, labelled with its display, for the Phase 2 multistream tests.
.DESCRIPTION
  Covers the given rectangle (desktop coordinates) with a large label, a moving bar and a frame counter, so every
  stream has new frames to encode and its snapshot shows which display it came from. Only the bar band and the
  counter are repainted each tick, so the window can keep up at 60 fps even when several run at once. With
  -LogFile, it writes how many frames it painted each second (to tell the content's frame rate from the stream's).
  Runs until closed or killed.
#>
param(
    [int]$X,
    [int]$Y,
    [int]$Width,
    [int]$Height,
    [string]$Label,
    [switch]$Static,  # no animation (the "typical" case's quiet displays)
    [string]$LogFile
)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# Coordinates are pixels, as the harness reads them
Add-Type -Namespace Tessera -Name Dpi -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[void][Tessera.Dpi]::SetProcessDPIAware()
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'AboveNormal'

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

# The label is drawn once into a bitmap; each paint only copies the part it needs
$fontSize = [float]([Math]::Max(18, [Math]::Min($Height / 10, $Width / ([Math]::Max(1, $Label.Length) * 0.75))))
$font = New-Object System.Drawing.Font('Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold)
$small = New-Object System.Drawing.Font('Consolas', 20)
$background = New-Object System.Drawing.Bitmap($Width, $Height)
$g0 = [System.Drawing.Graphics]::FromImage($background)
$g0.Clear($form.BackColor)
$g0.DrawString($Label, $font, [System.Drawing.Brushes]::White, 40, 40)
$g0.Dispose()

$barWidth = [int][Math]::Max(40, $Width / 10)
$band = New-Object System.Drawing.Rectangle(0, [int]($Height / 2), $Width, [int]($Height / 6))
$counter = New-Object System.Drawing.Rectangle(40, $Height - 90, 600, 60)
$script:frame = 0
$script:painted = 0

$form.Add_Paint({
    param($s, $e)
    $g = $e.Graphics
    $g.DrawImage($background, $e.ClipRectangle, $e.ClipRectangle, [System.Drawing.GraphicsUnit]::Pixel)
    $x = ($script:frame * 12) % ($Width + $barWidth) - $barWidth
    $g.FillRectangle([System.Drawing.Brushes]::OrangeRed, $x, $band.Y, $barWidth, $band.Height)
    $g.DrawString(("frame {0}" -f $script:frame), $small, [System.Drawing.Brushes]::LightGray, $counter.X, $counter.Y)
    $script:painted++
})

if (-not $Static) {
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 10
    $timer.Add_Tick({
        $script:frame++
        $form.Invalidate($band)
        $form.Invalidate($counter)
    })
    $timer.Start()
}

if ($LogFile) {
    $logTimer = New-Object System.Windows.Forms.Timer
    $logTimer.Interval = 1000
    $logTimer.Add_Tick({
        Add-Content -Path $LogFile -Value $script:painted
        $script:painted = 0
    })
    $logTimer.Start()
}
[System.Windows.Forms.Application]::Run($form)
