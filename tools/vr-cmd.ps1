# vr-cmd.ps1 - send seam commands to a game that is being played IN A HEADSET.
#
# WHY THIS EXISTS ALONGSIDE game-cmd.ps1. game-cmd.ps1 always calls
# SetForegroundWindow before writing. That is right for a flat harness run and
# wrong for a headset session: stealing focus can drop the XR session out of
# FOCUSED (session 33), at which point the mod publishes an inactive pad, the
# controllers go dead, and the game may pause - in the middle of the thing you
# were trying to tune. It has no -NoFocus switch, so this is the no-focus twin.
#
# The mod polls %LOCALAPPDATA%\BioshockVR\command.txt at 1 Hz on the game
# thread and applies every line when the file's write time changes, so writing
# the file is the whole mechanism. No focus change is needed or wanted.
#
# It also CONFIRMS the command landed, which game-cmd.ps1 does not: it watches
# the log for new lines and reports what appeared. A command that silently did
# nothing - because the game was at the title screen, or paused, or the verb was
# a typo - otherwise looks identical to one that worked, and that has already
# cost one debugging session here.
#
# Usage:
#   .\tools\vr-cmd.ps1 "headoff up 25"
#   .\tools\vr-cmd.ps1 "vraim laser on" "vraim muzzle on"
#   .\tools\vr-cmd.ps1 -Quiet "recenter"
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1",
    [double]$WaitSec = 4.0,
    [switch]$Quiet,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Lines
)

$ErrorActionPreference = 'Stop'

if (-not $Lines -or $Lines.Count -eq 0) { throw "no commands given." }

$dir = if ($Game -eq "bs2") { "$env:LOCALAPPDATA\BioshockVR\bs2" }
       else { "$env:LOCALAPPDATA\BioshockVR" }
$cmd = Join-Path $dir "command.txt"
$log = Join-Path $dir "bioshockvr.log"

$proc = if ($Game -eq "bs2") { "Bioshock2HD" } else { "BioshockHD" }
$p = @(Get-Process $proc -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited })
if ($p.Count -eq 0) {
    Write-Warning "$proc is not running - the file will be written and applied at the NEXT launch."
    Write-Warning "Note that tools\launch-game.ps1 and xrsim-launch.ps1 DELETE command.txt in preflight;"
    Write-Warning "only a direct/Steam launch will see it."
}

# Where the log ends now, so we can report only what this command produced.
$before = 0
if (Test-Path $log) { $before = (Get-Item $log).Length }

[System.IO.File]::WriteAllText($cmd, (($Lines -join "`n") + "`n"),
                               [System.Text.Encoding]::ASCII)
if (-not $Quiet) { Write-Output ("sent: " + ($Lines -join " | ")) }

if ($p.Count -eq 0 -or $WaitSec -le 0) { return }

# The poll is 1 Hz on the game thread, so anything under ~2 s is a false negative.
$deadline = (Get-Date).AddSeconds($WaitSec)
$seen = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    if (-not (Test-Path $log)) { continue }
    if ((Get-Item $log).Length -gt $before) { $seen = $true; break }
}

if (-not $seen) {
    Write-Warning "no new log output in ${WaitSec}s - the command may not have been applied."
    Write-Warning "The seam only polls while CalcView runs: at the title screen, or while the"
    Write-Warning "game is paused or unfocused, commands sit unapplied until gameplay resumes."
    return
}

if (-not $Quiet) {
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($before, 'Begin') | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        $new = $sr.ReadToEnd() -split "`r?`n" | Where-Object { $_ }
    } finally { $fs.Dispose() }
    # Echo the lines the mod logged in response, skipping the routine per-second
    # telemetry that would otherwise bury them.
    $new | Where-Object { $_ -notmatch '\[reentry\]|input drive:|camera: loc=|\[vrscale\]' } |
        Select-Object -Last 12 | ForEach-Object { "  $_" }
}
