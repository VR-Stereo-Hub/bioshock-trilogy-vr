# Copies the built mod DLLs into the game folder. Backs up any pre-existing
# xinput1_3.dll (e.g. the headtracking mod) as xinput1_3.dll.bvr-backup once.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [switch]$Release,
    [string]$GamePath = "K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$config = if ($Release) { "RelWithDebInfo" } else { "Debug" }
$outDir = Join-Path $repo "build\src\$config"

if (-not (Test-Path (Join-Path $GamePath "BioshockHD.exe"))) {
    throw "BioshockHD.exe not found in '$GamePath' - pass -GamePath."
}

$proxy = Join-Path $outDir "xinput1_3.dll"
$mod = Join-Path $outDir "bioshockvr.dll"
if (-not (Test-Path $proxy) -or -not (Test-Path $mod)) {
    throw "Build output missing in $outDir - run tools\build.ps1 first."
}

# Back up a pre-existing proxy exactly once, and only if it isn't already ours
$existing = Join-Path $GamePath "xinput1_3.dll"
$backup = Join-Path $GamePath "xinput1_3.dll.bvr-backup"
if ((Test-Path $existing) -and -not (Test-Path $backup)) {
    $ours = Test-Path (Join-Path $GamePath "bioshockvr.dll")
    if (-not $ours) {
        Copy-Item $existing $backup
        Write-Host "Backed up existing xinput1_3.dll -> xinput1_3.dll.bvr-backup"
    }
}

Copy-Item $proxy (Join-Path $GamePath "xinput1_3.dll") -Force
Copy-Item $mod (Join-Path $GamePath "bioshockvr.dll") -Force
Write-Host "Installed $config build to $GamePath"
Write-Host "Log will appear at $env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
