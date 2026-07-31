# launch-game.ps1 - launch BS1/BS2 through Steam, with the checks that keep a
# test run honest. USE THIS instead of a bare Start-Process steam://.
#
# It exists because a check that only PRINTS is not a check (session 33): the
# pre-launch "is BioShock Infinite running" warning was a Write-Output next to
# an unconditional Start-Process, so it duly reported that Infinite was up and
# launched anyway, on top of another session's work. This one throws.
#
# Guards, in order:
#   1. ANOTHER BioShock is running. A parallel session may own the machine, the
#      GPU and the headset (user directive, 2026-07-31). Refuses; -Force skips.
#   2. A stale command.txt. It re-applies at boot, because the poller's cached
#      write time starts zeroed - so last session's `gfov 130` silently returns.
#      Cleared automatically.
#
# Usage:
#   .\tools\launch-game.ps1 -Game bs2
#   .\tools\launch-game.ps1 -Game bs1 -Force      # I know, launch anyway
[CmdletBinding()]
param(
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1",
    [switch]$Force,
    [int]$WaitSeconds = 60
)

$ErrorActionPreference = 'Stop'

$appId = if ($Game -eq "bs2") { "409720" } else { "409710" }
$proc  = if ($Game -eq "bs2") { "Bioshock2HD" } else { "BioshockHD" }
$dir   = if ($Game -eq "bs2") { "$env:LOCALAPPDATA\BioshockVR\bs2" }
         else { "$env:LOCALAPPDATA\BioshockVR" }

# --- guard 1: another BioShock already running -------------------------------
$others = Get-Process -ErrorAction SilentlyContinue |
          Where-Object { $_.ProcessName -match '^(Bioshock|BioShockInfinite)' -and
                         $_.ProcessName -ne $proc }
if ($others) {
    $names = ($others | ForEach-Object { "$($_.ProcessName) (pid $($_.Id))" }) -join ', '
    if (-not $Force) {
        throw "REFUSING TO LAUNCH: $names is running. A parallel session may be " +
              "using that game, the GPU and the headset. Wait for it to close, or " +
              "re-run with -Force if you know it is safe."
    }
    Write-Warning "$names is running - launching anyway because -Force was given."
}

if (Get-Process $proc -ErrorAction SilentlyContinue) {
    Write-Output "$proc is already running - nothing to do."
    return
}

# --- guard 2: stale command file ---------------------------------------------
$cmd = Join-Path $dir "command.txt"
if (Test-Path $cmd) {
    $stale = (Get-Content $cmd -Raw).Trim()
    Remove-Item $cmd -Force
    if ($stale) { Write-Output "cleared a stale command.txt (would have re-applied at boot): $stale" }
}

Write-Output "launching $Game (appid $appId)..."
Start-Process "steam://rungameid/$appId"

for ($i = 0; $i -lt $WaitSeconds; $i++) {
    Start-Sleep -Seconds 1
    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne 0) {
        Write-Output "$proc up (pid $($p.Id)). Log: $dir\bioshockvr.log"
        return
    }
}
throw "$proc did not appear within $WaitSeconds s."
