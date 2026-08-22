# Removes the mod from the game folder; restores a backed-up xinput1_3.dll if
# one exists. Touches only files we put there.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$GamePath = ""
)

$ErrorActionPreference = "Stop"

if (-not $GamePath) {
    switch ($Game) {
        default {
            . (Join-Path $PSScriptRoot "lib\resolve-game-path.ps1")
            $GamePath = Resolve-BvrGamePath -Game $Game
        }
    }
}

$mod = Join-Path $GamePath "bioshockvr.dll"
$proxy = Join-Path $GamePath "xinput1_3.dll"
$backup = Join-Path $GamePath "xinput1_3.dll.bvr-backup"

if (Test-Path $mod) {
    Remove-Item $mod -Force
    Write-Host "Removed bioshockvr.dll"
    if (Test-Path $proxy) {
        Remove-Item $proxy -Force
        Write-Host "Removed xinput1_3.dll (our proxy)"
    }
} elseif (Test-Path $proxy) {
    Write-Host "xinput1_3.dll present but bioshockvr.dll is not - refusing to delete a proxy that may not be ours."
}

if (Test-Path $backup) {
    Move-Item $backup $proxy -Force
    Write-Host "Restored original xinput1_3.dll from backup"
}

Write-Host "Done."
