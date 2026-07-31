# xrsim-cmd.ps1 - send control lines to the simulated runtime and WAIT for them
# to be applied.
#
# Unlike game-cmd.ps1 this does not foreground the game: the sim polls on its own
# thread, and with an XR session live the mod is not paused when unfocused.
#
# It also waits for an acknowledgement instead of firing and hoping. The sim
# counts applied lines in state.json's cmdSeq, so a command that was never
# applied - or was rejected as a typo - fails here rather than silently doing
# nothing and being blamed on the mod three steps later.
#
# PositionalBinding is off for the reason game-cmd.ps1 already documents: as a
# positional parameter, -Dir would swallow the first command.
#
# Usage:
#   .\tools\xrsim-cmd.ps1 "head rot 35 0 0"
#   .\tools\xrsim-cmd.ps1 "reset" "hand r point 0 0" "trigger r 1.0"
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [double]$TimeoutSec = 5,
    [switch]$NoAck,
    [switch]$Quiet,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Lines
)

$ErrorActionPreference = 'Stop'

if (-not $Lines -or $Lines.Count -eq 0) { throw "no commands given." }
$statePath = Join-Path $Dir "state.json"
$cmdPath   = Join-Path $Dir "command.txt"

function Read-State {
    param([int]$Retries = 5)
    for ($k = 0; $k -lt $Retries; $k++) {
        try { return Get-Content $statePath -Raw -ErrorAction Stop | ConvertFrom-Json }
        catch { Start-Sleep -Milliseconds 120 }
    }
    return $null
}

$before = 0
if (-not $NoAck) {
    $s = Read-State
    if (-not $s) {
        throw "no readable state.json at $statePath - is a sim session live? " +
              "Launch with .\tools\xrsim-launch.ps1."
    }
    $before = [int]$s.cmdSeq
}

# One write for the whole batch, so the sim applies it as a single atomic rig
# change at one frame boundary. Retry past the poller's transient share lock,
# the same way game-cmd.ps1 does.
$text = ($Lines -join "`n") + "`n"
$written = $false
for ($i = 0; $i -lt 30; $i++) {
    try { [System.IO.File]::WriteAllText($cmdPath, $text); $written = $true; break }
    catch { Start-Sleep -Milliseconds 200 }
}
if (-not $written) { throw "could not write $cmdPath after 6 s (locked by the poller?)." }

if ($NoAck) { return }

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$want = $before + $Lines.Count
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $s = Read-State -Retries 2
    if ($s -and [int]$s.cmdSeq -ge $want) {
        if ($s.lastCmdError) {
            throw "the simulator rejected a command: $($s.lastCmdError) (last: '$($s.lastCmd)')"
        }
        if (-not $Quiet) {
            Write-Host "applied $($Lines.Count) command(s), cmdSeq $before -> $($s.cmdSeq), frame $($s.frame)"
        }
        return $s
    }
}
throw "no acknowledgement within $TimeoutSec s (cmdSeq still below $want). " +
      "Is the session live and are frames advancing?"
