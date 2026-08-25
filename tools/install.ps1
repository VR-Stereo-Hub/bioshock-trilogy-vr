# Copies the built mod DLLs into the game folder. Backs up any pre-existing
# xinput1_3.dll (e.g. the headtracking mod) as xinput1_3.dll.bvr-backup once.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
# Deliberately NOT guarded against a conflicting game: copying files touches the
# disk, never the headset, so installing works while any game is running.
param(
    [switch]$Release,
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$GamePath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$config = if ($Release) { "RelWithDebInfo" } else { "Debug" }
$outDir = Join-Path $repo "build\src\$config"

. (Join-Path $PSScriptRoot "lib\resolve-game-path.ps1")
$GamePath = Resolve-BvrGamePath -Game $Game -GamePath $GamePath

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

# s62: the SteamVR shim runtime + Valve's OpenVR loader, when built. Optional
# by design - without them the mod simply has no SteamVR fallback.
$shim = Join-Path $outDir "bvr_steamvr32.dll"
$ovr = Join-Path $repo "third_party\openvr_headers\bin\win32\openvr_api.dll"
if (Test-Path $shim) {
    Copy-Item $shim (Join-Path $GamePath "bvr_steamvr32.dll") -Force
    Copy-Item $ovr (Join-Path $GamePath "openvr_api.dll") -Force
    Write-Host "Installed SteamVR shim (bvr_steamvr32.dll + openvr_api.dll)"
}
Write-Host "Installed $config build to $GamePath"
switch ($Game) {
    "bs2" { Write-Host "Log will appear at $env:LOCALAPPDATA\BioshockVR\bs2\bioshockvr.log" }
    "bsi" { Write-Host "Log will appear at $env:LOCALAPPDATA\BioshockVR\bsi\bioshockvr.log" }
    default { Write-Host "Log will appear at $env:LOCALAPPDATA\BioshockVR\bioshockvr.log" }
}
