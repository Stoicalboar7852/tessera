<#
.SYNOPSIS
  Builds the Qt client locally with Qt 6.12 and the EWDK's MSVC, and deploys it to a folder that runs portable.

.DESCRIPTION
  For quick iterations on the dev PC; CI (.github/workflows/ci-client-qt.yml, scripts\build-arch.bat) stays the
  reference build. One-time setup:
    - Qt:   T:\toolchain\aqt-venv\Scripts\python.exe -m aqt install-qt windows desktop 6.12.0 win64_msvc2022_64
              --outputdir T:\toolchain\qt   (aqtinstall at the commit CI pins, in a CPython venv)
    - jom:  https://download.qt.io/official_releases/jom/jom.zip extracted to T:\toolchain\jom
    - deps: clients\qt\setup-deps.ps1
  The EWDK's build environment sets INCLUDE and LIB for MSVC only, so this adds the Windows SDK paths.
  Output: T:\build\qt-client (objects) and T:\build\qt-client-<config> (Moonlight.exe with its DLLs and portable.dat).
  -Tests builds and runs the Tessera unit tests (clients\qt\tests\tessera) in T:\build\qt-client-tests instead.

.EXAMPLE
  .\tools\setup\build-qt-client.ps1
  .\tools\setup\build-qt-client.ps1 -Config debug -Reconfigure
  .\tools\setup\build-qt-client.ps1 -Tests
#>
param(
    [ValidateSet('release', 'debug')] [string]$Config = 'release',
    [switch]$Reconfigure,
    [switch]$NoDeploy,
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1" | Out-Null

$qt = 'T:\toolchain\qt\6.12.0\msvc2022_64'
$jom = 'T:\toolchain\jom'
$source = 'T:\src\clients\qt'
$build = if ($Tests) { 'T:\build\qt-client-tests' } else { 'T:\build\qt-client' }
$deploy = "T:\build\qt-client-$Config"
foreach ($required in "$qt\bin\qmake.exe", "$jom\jom.exe", "$source\libs\windows\lib\x64\SDL2.dll") {
    if (-not (Test-Path $required)) { throw "missing $required (see the one-time setup in this script's help)" }
}
New-Item -ItemType Directory -Force $build | Out-Null
if ($Reconfigure -and (Test-Path "$build\Makefile")) { Remove-Item "$build\Makefile" }

# A batch file, so the EWDK's variables are expanded after SetupBuildEnv has set them
$sdk = '%WindowsSdkDir%Include\%Version_Number%'
$sdkLib = '%WindowsSdkDir%Lib\%Version_Number%'
if ($Tests) {
    $steps = @"
if not exist Makefile (
    qmake.exe $source\tests\tessera\tessera-tests.pro "CONFIG+=$Config" "CONFIG-=debug_and_release" || exit /b 1
)
jom.exe || exit /b 1
"@
}
else {
    $steps = @"
if not exist Makefile (
    qmake.exe $source\moonlight-qt.pro || exit /b 1
)
jom.exe $Config || exit /b 1
if not exist "$deploy" mkdir "$deploy"
copy /y "%VCToolsRedistDir%x64\Microsoft.VC145.CRT\*.dll" "$deploy" >nul || exit /b 1
"@
}
$batch = @"
@echo off
set "INCLUDE=%INCLUDE%;$sdk\ucrt;$sdk\um;$sdk\shared;$sdk\winrt;$sdk\cppwinrt"
set "LIB=%LIB%;$sdkLib\ucrt\x64;$sdkLib\um\x64"
set "PATH=$qt\bin;$jom;%PATH%"
cd /d $build
$steps
"@
Set-Content -Path "$build\build.cmd" -Value $batch -Encoding ascii
& "$PSScriptRoot\ewdk.ps1" "call `"$build\build.cmd`" > `"$build\build.log`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Get-Content "$build\build.log" | Select-String -Pattern ': (fatal )?error ' | Select-Object -First 30 | ForEach-Object { $_.Line }
    throw "build failed ($LASTEXITCODE), see $build\build.log"
}
if ($Tests) {
    $env:PATH = "$qt\bin;$source\libs\windows\lib\x64;$env:PATH"
    # QtTest's own output doesn't reach a PowerShell pipe, so it goes to a file; SDL's log on stderr is dropped
    cmd /c "`"$build\tessera-tests.exe`" -o `"$build\results.txt,txt`" 2>nul"
    $exitCode = $LASTEXITCODE
    Get-Content "$build\results.txt"
    if ($exitCode -ne 0) { throw "tests failed ($exitCode)" }
    return
}
if ($NoDeploy) { return }

# The same deployment as scripts\build-arch.bat, without the installer
Copy-Item "$build\app\$Config\Moonlight.exe" $deploy -Force
Copy-Item "$source\libs\windows\lib\x64\*.dll" $deploy -Force
Copy-Item "$build\AntiHooking\$Config\AntiHooking.dll" $deploy -Force
Copy-Item "$source\app\SDL_GameControllerDB\gamecontrollerdb.txt" $deploy -Force
$skip = '--no-system-d3d-compiler', '--no-system-dxc-compiler', '--skip-plugin-types', 'qmltooling,generic', '--no-ffmpeg',
    '--no-quickcontrols2fusion', '--no-quickcontrols2imagine', '--no-quickcontrols2universal',
    '--no-quickcontrols2fusionstyleimpl', '--no-quickcontrols2imaginestyleimpl', '--no-quickcontrols2universalstyleimpl',
    '--no-quickcontrols2windowsstyleimpl', '--no-quickcontrols2fluentwinui3styleimpl'
& "$qt\bin\windeployqt.exe" --dir $deploy "--$Config" --qmldir "$source\app\gui" --no-opengl-sw --no-compiler-runtime --no-sql @skip "$deploy\Moonlight.exe" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
# Settings and logs stay next to the executable (Moonlight looks for portable.dat in its working directory)
Set-Content -Path "$deploy\portable.dat" -Value '' -Encoding ascii
Write-Host "Built $deploy\Moonlight.exe"
