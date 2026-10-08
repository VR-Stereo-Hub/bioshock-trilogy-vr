# xrsim-run.ps1 - run a scripted sim sequence (.xrs) and return its captures.
#
# A .xrs file is sim commands, one per line, with a few directives:
#   <cmd>[; <cmd>]        sim commands; ';' sends them as ONE atomic batch (same frame)
#   #  ...                comment
#   @wait <ms>            sleep
#   @frames <n>           wait for the sim's frame counter to advance n
#   @shot <name>          capture; the result object is collected and returned
#   @mod <cmd>[; <cmd>]   route to the MOD's command.txt via game-cmd.ps1.
#                         Group with ';' to land in ONE poll, and note that this
#                         then WAITS a poll period: the mod reads command.txt at
#                         1 Hz on mtime change, so back-to-back @mod lines
#                         overwrite each other before it ever sees them. That is
#                         the same trap game-batch.ps1 documents, and it silently
#                         cost a `vraim on` in the first version of this file.
#   @key <name> [n] [ms]  press a keyboard key in the GAME window (game-key.ps1:
#                         scancode injection; foregrounds the game).
#   @mark                 forget the mod log written so far: @log and @nolog only
#                         look at what the mod wrote AFTER the last mark (the
#                         start of the sequence is the first mark).
#   @log <regex> [<n>s]   wait up to n seconds (default 5) for a line of the MOD's
#                         log, written since the mark, to match. A match moves the
#                         mark past it, so consecutive @log lines assert an ORDER.
#   @nolog <regex>        fail if a line since the mark matches (a swing that must
#                         NOT fire). Does not move the mark.
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
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [string]$OutDir = "",
    [double]$Delay = 0,
    # One mod poll period (1 Hz) plus margin. Anything shorter and consecutive
    # @mod writes race the poller.
    [int]$ModPollMs = 1400,
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
$keyScript   = Join-Path $PSScriptRoot "game-key.ps1"

# The mod's log, read shared (the game holds it open) from a byte mark. Same
# per-game paths as tail-log.ps1. @key/@mark/@log/@nolog and the ';' batch are
# ported from the Dishonored VR mod's copy of this runner.
$modLog = switch ($Game) {
    "bs2"   { "$env:LOCALAPPDATA\BioshockVR\bs2\bioshockvr.log" }
    "bsi"   { "$env:LOCALAPPDATA\BioshockVR\bsi\bioshockvr.log" }
    default { "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log" }
}
$script:logMark = if (Test-Path $modLog) { (Get-Item $modLog).Length } else { 0 }
function Read-ModLogSince([long]$from) {
    if (-not (Test-Path $modLog)) { return "" }
    $fs = [IO.File]::Open($modLog, 'Open', 'Read', 'ReadWrite')
    try {
        if ($from -gt $fs.Length) { $from = 0 }      # the log rotated under us
        $fs.Seek($from, 'Begin') | Out-Null
        # Latin-1 on purpose: one byte is one character, so an offset into the text
        # IS an offset into the file. Decoded as UTF-8, a stray high byte in the log
        # becomes a 3-byte replacement character, the mark overshoots the file, and
        # the next read starts from the top and matches lines from before the mark.
        $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::GetEncoding(28591))
        return $sr.ReadToEnd()
    } finally { $fs.Close() }
}

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
                # Semicolon-grouped commands go in one write so they land in the
                # same poll; then wait past the mod's 1 Hz poller before the next
                # write can clobber them.
                $modCmds = @($Matches[1] -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
                & $gameCmd -Game $Game @modCmds | Out-Null
                Start-Sleep -Milliseconds $ModPollMs
            }
            elseif ($line -match '^@key\s+(\S+)(?:\s+(\d+))?(?:\s+(\d+))?') {
                $rep = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
                $del = if ($Matches[3]) { [int]$Matches[3] } else { 500 }
                & $keyScript -Game $Game -Key $Matches[1] -Repeat $rep -Delay $del | Out-Null
            }
            elseif ($line -match '^@mark\s*$') {
                $script:logMark = if (Test-Path $modLog) { (Get-Item $modLog).Length } else { 0 }
            }
            elseif ($line -match '^@log\s+(.+?)(?:\s+(\d+)s)?$') {
                $rx = $Matches[1]; $secs = if ($Matches[2]) { [int]$Matches[2] } else { 5 }
                $deadline = (Get-Date).AddSeconds($secs); $hit = $null; $text = ""
                do {
                    $text = Read-ModLogSince $script:logMark
                    $hit = [regex]::Match($text, $rx)
                    if ($hit.Success) { break }
                    Start-Sleep -Milliseconds 250
                } while ((Get-Date) -lt $deadline)
                if (-not $hit.Success) { throw "LOG FAILED: no mod log line matched /$rx/ within ${secs}s of the mark" }
                $eol = $text.IndexOf("`n", $hit.Index); if ($eol -lt 0) { $eol = $text.Length }
                $bol = $text.LastIndexOf("`n", [Math]::Max(0, $hit.Index - 1)) + 1
                Write-Host "      matched: $($text.Substring($bol, $eol - $bol).Trim())"
                $script:logMark += $eol
            }
            elseif ($line -match '^@nolog\s+(.+)$') {
                $rx = $Matches[1]
                $hit = [regex]::Match((Read-ModLogSince $script:logMark), $rx)
                if ($hit.Success) { throw "NOLOG FAILED: the mod log matched /$rx/ since the mark: $($hit.Value)" }
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
                # ';' groups sim commands into ONE atomic batch, applied on the same
                # frame: a head and a hand that must move TOGETHER cannot be two writes.
                $simCmds = @($line -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
                & $cmdScript -Dir $Dir -Quiet @simCmds | Out-Null
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
