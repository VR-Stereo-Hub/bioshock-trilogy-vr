# Follows the mod log live. -Game bs2 / bsi follow the BioShock 2 / Infinite log
# (per-game data subdir; see src/core/util/log.cpp).
# Read-only, so it is deliberately NOT guarded against a conflicting game: tailing
# a log never touches the headset.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1")
switch ($Game) {
    "bs2" { $log = "$env:LOCALAPPDATA\BioshockVR\bs2\bioshockvr.log" }
    "bsi" { $log = "$env:LOCALAPPDATA\BioshockVR\bsi\bioshockvr.log" }
    default { $log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log" }
}
if (-not (Test-Path $log)) {
    Write-Host "No log yet at $log - launch the game with the mod installed first."
    Write-Host "Waiting for it to appear..."
    while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 500 }
}
Get-Content $log -Wait -Tail 50
