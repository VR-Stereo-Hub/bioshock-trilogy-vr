# Follows the mod log live. -Game bs2 follows the BioShock 2 log (per-game
# data subdir; see src/core/util/log.cpp).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([ValidateSet("bs1", "bs2")][string]$Game = "bs1")
if ($Game -eq "bs2") {
    $log = "$env:LOCALAPPDATA\BioshockVR\bs2\bioshockvr.log"
} else {
    $log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
}
if (-not (Test-Path $log)) {
    Write-Host "No log yet at $log - launch the game with the mod installed first."
    Write-Host "Waiting for it to appear..."
    while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 500 }
}
Get-Content $log -Wait -Tail 50
