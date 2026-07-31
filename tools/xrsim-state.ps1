# xrsim-state.ps1 - read state.json, or wait for a condition in it.
#
# The -For predicate mini-language has exactly two forms:
#   key=value   wait until the field equals that value  ("sessionState=FOCUSED")
#   key+N       wait until the field has advanced N from its value at entry
#               ("frame+30", "captureSeq+1")
#
# Usage:
#   .\tools\xrsim-state.ps1
#   .\tools\xrsim-state.ps1 -For "sessionState=FOCUSED"
#   .\tools\xrsim-state.ps1 -For "frame+60" -TimeoutSec 5
[CmdletBinding()]
param(
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [string]$For = "",
    [double]$TimeoutSec = 15,
    [switch]$Raw,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$statePath = Join-Path $Dir "state.json"

function Read-State {
    param([int]$Retries = 5)
    for ($k = 0; $k -lt $Retries; $k++) {
        # A torn read is possible in principle even with the writer's atomic
        # replace, so retry before declaring a parse failure. If it never
        # settles, that is a WRITER bug, not a reader bug - say so.
        try { return Get-Content $statePath -Raw -ErrorAction Stop | ConvertFrom-Json }
        catch { Start-Sleep -Milliseconds 120 }
    }
    return $null
}

$s = Read-State
if (-not $s) {
    throw "could not read a valid $statePath - no sim session, or the writer " +
          "produced malformed JSON."
}

if ($For) {
    if ($For -match '^\s*([A-Za-z][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$') {
        $key = $Matches[1]; $want = $Matches[2]
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            if ("$($s.$key)" -eq $want) { break }
            Start-Sleep -Milliseconds 100
            $t = Read-State -Retries 2
            if ($t) { $s = $t }
        }
        if ("$($s.$key)" -ne $want) {
            throw "timed out waiting for '$For' after $TimeoutSec s (last: $key = $($s.$key), frame $($s.frame))"
        }
    }
    elseif ($For -match '^\s*([A-Za-z][A-Za-z0-9_]*)\s*\+\s*(\d+)\s*$') {
        $key = $Matches[1]; $delta = [int]$Matches[2]
        $start = [int64]$s.$key
        $target = $start + $delta
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            if ([int64]$s.$key -ge $target) { break }
            Start-Sleep -Milliseconds 100
            $t = Read-State -Retries 2
            if ($t) { $s = $t }
        }
        if ([int64]$s.$key -lt $target) {
            throw "timed out waiting for '$For' after $TimeoutSec s ($key reached $($s.$key), wanted $target). " +
                  "Frames not advancing? Check paceMode - step mode with no credits stops the clock."
        }
    }
    else {
        throw "cannot parse -For '$For'. Use 'key=value' or 'key+N'."
    }
}

if ($Raw) { return $s }

if (-not $Quiet) {
    Write-Host ("frame {0} | state {1} | layers {2} (proj views {3}) | pace {4} {5:N1} Hz, {6} pending | cmdSeq {7} | errors {8}" -f `
        $s.frame, $s.sessionState, $s.layersLastFrame, $s.projectionViews, $s.paceMode, `
        $s.refreshHz, $s.stepsPending, $s.cmdSeq, $s.errors)
}
$s
