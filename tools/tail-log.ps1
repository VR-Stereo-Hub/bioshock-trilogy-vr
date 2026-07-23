# Follows the mod log live.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
if (-not (Test-Path $log)) {
    Write-Host "No log yet at $log - launch the game with the mod installed first."
    Write-Host "Waiting for it to appear..."
    while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 500 }
}
Get-Content $log -Wait -Tail 50
