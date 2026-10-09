param([switch]$Tests)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmake = & $vswhere -latest -products '*' -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
if (-not $cmake) { throw 'Visual Studio CMake tools were not found.' }
$install = & $vswhere -latest -products '*' -property installationVersion | Select-Object -First 1
$generator = if ($install -match '^18\.') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
if (-not (Test-Path -LiteralPath (Join-Path $repo 'assets\ui\launcher\icons\bioshock-vr-medallion.ico'))) { & "$PSScriptRoot\launcher-icons.ps1" }
& $cmake -S $repo -B "$repo\build" -G $generator -A Win32
if ($LASTEXITCODE -ne 0) { throw 'Launcher configure failed.' }
$targets = @('bvr_launcher')
if ($Tests) { $targets += 'bvr_launcher_tests' }
& $cmake --build "$repo\build" --config RelWithDebInfo --target $targets --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }
