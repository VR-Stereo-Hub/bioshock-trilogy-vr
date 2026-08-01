# xrsim-run.ps1 - run a scripted sim sequence (.xrs) and return its captures.
#
# A .xrs file is sim commands, one per line, with a few directives:
#   #  ...                comment
#   @wait <ms>            sleep
#   @frames <n>           wait for the sim's frame counter to advance n
#   @shot <name>          capture; the result object is collected and returned
#   @mod <seam command>   route to the MOD's command.txt via game-cmd.ps1
#   @assert <k> <op> <v>  assert on state.json (ops: eq ne gt ge lt le)
#   @fps <min> [secs]     measure frames/s over a window and fail below <min>.
#                         This is the session-33 oracle: the symptom there was a
#                         frame-rate COLLAPSE, which no state field records.
#
# Usage:
#   .\tools\xrsim-run.ps1 -Path .\tools\xrsim\smoke.xrs
#   $shots = .\tools\xrsim-run.ps1 -Steps "reset","head rot 30 0 0","@shot a"
[CmdletBinding()]
param(
    [string]$Path = "",
    [string[]]$Steps = @(),
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1",
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [string]$OutDir = "",
    [double]$Delay = 0,
    [switch]$ContinueOnError
)

$ErrorActionPreference = 'Stop'

if ($Path) {
    if (-not (Test-Path $Path)) { throw "sequence not found: $Path" }
    $Steps = @(Get-Content $Path)
}
if (-not $Steps -or $Steps.Count -eq 0) { throw "nothing to run - pass -Path or -Steps." }

$cmdScript   = Join-Path $PSScriptRoot "xrsim-cmd.ps1"
$stateScript = Join-Path $PSScriptRoot "xrsim-state.ps1"
$shotScript  = Join-Path $PSScriptRoot "xrsim-shot.ps1"
$gameCmd     = Join-Path $PSScriptRoot "game-cmd.ps1"

$shots = @()
$n = 0
$total = ($Steps | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }).Count

try {
    foreach ($raw in $Steps) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith('#')) { continue }
        $n++
        Write-Host "[$n/$total] $line"

        try {
            if ($line -match '^@wait\s+(\d+)') {
                Start-Sleep -Milliseconds ([int]$Matches[1])
            }
            elseif ($line -match '^@frames\s+(\d+)') {
                & $stateScript -Dir $Dir -For "frame+$($Matches[1])" -TimeoutSec 30 -Quiet | Out-Null
            }
            elseif ($line -match '^@shot\s+(\S+)') {
                $name = $Matches[1]
                $out = if ($OutDir) { Join-Path $OutDir $name } else { "" }
                $shots += (& $shotScript -Dir $Dir -Out $out)
            }
            elseif ($line -match '^@mod\s+(.+)$') {
                & $gameCmd -Game $Game $Matches[1] | Out-Null
            }
            elseif ($line -match '^@fps\s+([\d.]+)(?:\s+([\d.]+))?') {
                $min = [double]$Matches[1]
                $secs = if ($Matches[2]) { [double]$Matches[2] } else { 3.0 }
                $a = & $stateScript -Dir $Dir -Quiet
                Start-Sleep -Seconds $secs
                $b = & $stateScript -Dir $Dir -Quiet
                $fps = [math]::Round(($b.frame - $a.frame) / $secs, 1)
                Write-Host "      measured $fps frames/s over ${secs}s (min $min)"
                if ($fps -lt $min) {
                    throw "FPS FAILED: $fps frames/s is below $min while state=$($b.sessionState)"
                }
            }
            elseif ($line -match '^@assert\s+(\S+)\s+(eq|ne|gt|ge|lt|le)\s+(.+)$') {
                $k = $Matches[1]; $op = $Matches[2]; $v = $Matches[3]
                $s = & $stateScript -Dir $Dir -Quiet
                $actual = $s.$k
                $ok = switch ($op) {
                    'eq' { "$actual" -eq $v }
                    'ne' { "$actual" -ne $v }
                    'gt' { [double]$actual -gt [double]$v }
                    'ge' { [double]$actual -ge [double]$v }
                    'lt' { [double]$actual -lt [double]$v }
                    'le' { [double]$actual -le [double]$v }
                }
                if (-not $ok) { throw "ASSERT FAILED: $k ($actual) $op $v" }
            }
            else {
                & $cmdScript -Dir $Dir -Quiet $line | Out-Null
            }
        } catch {
            if (-not $ContinueOnError) { throw }
            Write-Warning "step $n failed: $_"
        }

        if ($Delay -gt 0) { Start-Sleep -Seconds $Delay }
    }
} finally {
    # Never leave the sim gated on an agent that has stopped stepping.
    try { & $cmdScript -Dir $Dir -Quiet -TimeoutSec 3 "step off" | Out-Null } catch { }
}

Write-Host "sequence complete: $n step(s), $($shots.Count) capture(s)"
$shots
