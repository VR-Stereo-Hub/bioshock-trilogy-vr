# Builds the mod (32-bit). CMake is not on PATH on this machine - we use the one
# bundled with VS 2022 Build Tools, located via vswhere.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [switch]$Release,
    [switch]$Install,
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install VS 2022 Build Tools." }
$cmake = & $vswhere -latest -products * -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" | Select-Object -First 1
if (-not $cmake) { throw "VS-bundled CMake not found - install the 'C++ CMake tools' component." }

# cmake presets are resolved relative to the current directory
Push-Location $repo
try {
    if (-not (Test-Path "build\CMakeCache.txt")) {
        & $cmake --preset win32
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    }

    $preset = if ($Release) { "release" } else { "debug" }
    & $cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}
finally {
    Pop-Location
}

if ($Install) {
    & (Join-Path $PSScriptRoot "install.ps1") -Release:$Release -Game $Game
}
