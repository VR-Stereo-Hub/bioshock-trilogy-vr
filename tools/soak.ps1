# soak.ps1 - run ONE VR mode for N minutes and FAIL, with an exit code, if the
# game wedges. The acceptance instrument for the BS2 stereo freeze (session 35).
#
# Why it exists: every soak in this project's history was run by hand - launch,
# press Space, write command.txt, "wait, it wedges in 5-100 s", then eyeball
# three files. That is unrepeatable, unattendable, and it cannot bisect. This
# turns "did it freeze?" into an exit code.
#
# WHAT IT CHECKS, and why each one is here:
#   1. the process is alive                     - it can also crash, not only wedge
#   2. bioshockvr.log mtime is advancing        - the wedge signature: the log stops
#      dead and never resumes. Liveness is by the LOG, never by Process.Responding,
#      which reads False on a perfectly healthy VR run (session 33).
#   3. no new WATCHDOG lines in pacetrace.log   - the stall detector's verdict.
#      pacetrace has its OWN thread and its OWN file handle (_SH_DENYWR), so it
#      keeps writing while the game thread is wedged. That is the whole point.
#   4. no new crash dumps                       - a fix that converts the freeze
#      into a crash is not a fix (session 34 measured exactly that)
#
# AND IT REFUSES TO CALL A RUN CLEAN IF THE INSTRUMENT WAS NOT RUNNING. If
# pacetrace.log never appeared, check 3 was vacuous, and the run exits 7
# (inconclusive) rather than 0. A green light nobody could have failed is worse
# than a red one.
#
# Usage:
#   .\tools\soak.ps1 -Game bs2 -Minutes 10 -Arm "vrstereo on"
#   .\tools\soak.ps1 -Game bs2 -Minutes 5  -Arm "reentry srdev on; vrstereo on"
#   .\tools\soak.ps1 -Game bs2 -Minutes 10 -Arm none          # vanilla flat
#   .\tools\soak.ps1 -Game bs2 -Attach -Minutes 3 -Arm "vrcam on"
#   .\tools\soak.ps1 -Game bs2 -Boot map -Map Ghetto.bsm -Minutes 10 -Arm "vrstereo on"
#
# Exit codes (so a bisect or a CI-ish loop can branch on them):
#   0 pass      2 log stalled = WEDGE      3 WATCHDOG fired     4 process died
#   5 never reached gameplay   6 arm failed   7 inconclusive   8 new crash dump
#   9 preflight refused (another BioShock is running - a parallel session may own the machine)
[CmdletBinding()]
param(
    [ValidateSet("bs1", "bs2")][string]$Game = "bs2",
    # Command(s) to arm once gameplay is live. "" or "none" soaks vanilla.
    # Semicolons group commands into one write, as in game-batch.ps1.
    [string]$Arm = "",
    [double]$Minutes = 10,
    # Attach to an already-running game instead of launching one. This is the
    # floor when unattended boot does not work: a human reaches gameplay once,
    # and every soak after that attaches.
    [switch]$Attach,
    [ValidateSet("steam", "map", "key", "none")][string]$Boot = "steam",
    [string]$Map = "",
    [string]$GamePath = "",
    [string]$Label = "",
    [switch]$KillOnFail,
    # Pass in a headset/XR session: asserting foreground drops the XR session out
    # of FOCUSED, which is its own hazard (session 33).
    [switch]$NoFocus,
    [switch]$Force,
    [int]$GameplayTimeoutSeconds = 240
)

$ErrorActionPreference = 'Stop'

$proc = if ($Game -eq "bs2") { "Bioshock2HD" } else { "BioshockHD" }
$dir  = if ($Game -eq "bs2") { "$env:LOCALAPPDATA\BioshockVR\bs2" }
        else { "$env:LOCALAPPDATA\BioshockVR" }
$log      = Join-Path $dir "bioshockvr.log"
$trace    = Join-Path $dir "pacetrace.log"
$crashDir = Join-Path $dir "crash"
# The 1 Hz camera heartbeat only ticks once CalcView is running, i.e. in
# gameplay. At the menu there is no heartbeat, so a clock started there would
# report a wedge that is really just a title screen.
# NOT anchored: log lines begin with a [HH:mm:ss.fff] timestamp (session 36's
# first mapboot run counted zero beats through 4 minutes of live heartbeat).
$beatRe = if ($Game -eq "bs2") { '\[b2r\] camera:' } else { '\[b1r\] camera:' }

$tag = if ($Label) { "[$Label] " } else { "" }

function Write-Step([string]$m) { Write-Host ("{0}{1}" -f $tag, $m) }

# Read only the bytes appended since the last call. Share ReadWrite|Delete
# because the game holds both files open; pacetrace is _SH_DENYWR, which denies
# other WRITERS, not readers.
function Read-NewText([string]$path, [ref]$offset) {
    if (-not (Test-Path $path)) { return "" }
    $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read, $share)
    } catch { return "" }
    try {
        if ($fs.Length -lt $offset.Value) { $offset.Value = 0 }   # rotated under us
        [void]$fs.Seek([int64]$offset.Value, [System.IO.SeekOrigin]::Begin)
        $len = [int]([Math]::Min([int64]4MB, $fs.Length - $offset.Value))
        if ($len -le 0) { return "" }
        $buf = New-Object byte[] $len
        $read = $fs.Read($buf, 0, $len)
        $offset.Value = $offset.Value + $read
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $read)
    } finally { $fs.Dispose() }
}

function Get-DumpCount {
    if (-not (Test-Path $crashDir)) { return 0 }
    return @(Get-ChildItem $crashDir -Filter "bvr_*.dmp" -ErrorAction SilentlyContinue).Count
}

function Show-Tail([string]$path, [int]$n, [string]$what) {
    if (-not (Test-Path $path)) { Write-Host "  (${what}: no file)"; return }
    Write-Host "  --- last $n lines of $what ---"
    try {
        Get-Content $path -Tail $n -ErrorAction Stop |
            ForEach-Object { Write-Host "  $_" }
    } catch {
        Write-Host "  (could not read $what : $_)"
    }
}

function Stop-GameIfAsked {
    if (-not $KillOnFail) {
        Write-Host "  (left running - pass -KillOnFail to force-kill wedged runs)"
        return
    }
    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if ($p) {
        Write-Host "  force-killing $proc (pid $($p.Id)) so the next run can launch"
        Stop-Process -Id $p.Id -Force
        Start-Sleep -Seconds 3
    }
}

# --- launch or attach --------------------------------------------------------

# launch-game's guards THROW; translate them into exit codes. If the refusal is
# "this very game is already running", degrade to attach - a soak does not care
# who booted the process. Anything else (another BioShock, a parallel session's
# machine) is exit 9, never a launch.
function Invoke-Preflight {
    try {
        & (Join-Path $PSScriptRoot "launch-game.ps1") -Game $Game -PreflightOnly -Force:$Force |
            ForEach-Object { Write-Step $_ }
        return $true
    } catch {
        if (Get-Process $proc -ErrorAction SilentlyContinue) {
            Write-Step "$proc is already running - degrading to attach (a soak does not care who booted it)"
            return $false
        }
        Write-Step "FAIL: preflight refused: $($_.Exception.Message)"
        exit 9
    }
}

if ($Attach) {
    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if (-not $p) { Write-Step "FAIL: -Attach but $proc is not running"; exit 4 }
    Write-Step "attached to $proc (pid $($p.Id))"
} elseif ($Boot -eq "none") {
    # Launch NOTHING: wait for someone else (a human, another script) to start
    # the process, then behave like -Attach.
    Write-Step "boot none: launching nothing, waiting up to $GameplayTimeoutSeconds s for $proc..."
    $p = $null
    for ($i = 0; $i -lt $GameplayTimeoutSeconds; $i++) {
        $p = Get-Process $proc -ErrorAction SilentlyContinue
        if ($p) { break }
        Start-Sleep -Seconds 1
    }
    if (-not $p) { Write-Step "FAIL: $proc never appeared (-Boot none launches nothing)"; exit 4 }
    Write-Step "found $proc (pid $($p.Id))"
} elseif (Invoke-Preflight) {
    if ($Boot -eq "map") {
        if (-not $Map) { throw "-Boot map needs -Map <name.bsm>" }
        if (-not $GamePath) {
            $GamePath = if ($Game -eq "bs2") {
                "D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final"
            } else {
                "K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final"
            }
        }
        $exeName = if ($Game -eq "bs2") { "Bioshock2HD.exe" } else { "BioshockHD.exe" }
        $exe = Join-Path $GamePath $exeName
        if (-not (Test-Path $exe)) { throw "game exe not found: $exe" }
        Write-Step "launching $exeName directly with map URL '$Map'"
        $mp = Start-Process -FilePath $exe -WorkingDirectory $GamePath -ArgumentList $Map -PassThru
        Start-Sleep -Seconds 5
        if ($mp.HasExited) {
            # A DRM bounce relaunches through Steam and DROPS the argument; a
            # plain death leaves nothing. Either way the direct map boot failed.
            $p = Get-Process $proc -ErrorAction SilentlyContinue
            if ($p) {
                Write-Step "direct exe exited but $proc is up (pid $($p.Id)) - Steam bounce, the map arg is LOST; falling back to the title-screen key lane"
                $Boot = "key"
            } else {
                Write-Step "FAIL: $exeName exited within 5 s and nothing relaunched. Is the Steam client running?"
                exit 4
            }
        }
    } else {
        # steam | key: launch-game re-runs its own guards (idempotent, cheap)
        & (Join-Path $PSScriptRoot "launch-game.ps1") -Game $Game -Force:$Force |
            ForEach-Object { Write-Step $_ }
    }
}

# --- wait for GAMEPLAY, not merely for a process -----------------------------
# The revert-Options dialog blocks the game before it ever presents, so dismiss
# it here rather than making every caller remember to (same trick as
# xrsim-launch.ps1 and boot.ps1).
Add-Type -ErrorAction SilentlyContinue @'
using System; using System.Runtime.InteropServices;
public static class BvrSoakWin {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

$logOffset = [int64]0
$beats = 0
$keyTried = $false
$gameplay = $false

Write-Step "waiting for gameplay (the 1 Hz camera heartbeat), up to $GameplayTimeoutSeconds s..."
for ($i = 0; $i -lt $GameplayTimeoutSeconds; $i++) {
    Start-Sleep -Seconds 1

    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if (-not $p) { Write-Step "FAIL: $proc is not running while waiting for gameplay"; exit 4 }

    $dlg = [BvrSoakWin]::FindWindow("#32770", "Message")
    if ($dlg -ne [IntPtr]::Zero) {
        $no = [BvrSoakWin]::FindWindowEx($dlg, [IntPtr]::Zero, "Button", "&No")
        if ($no -ne [IntPtr]::Zero) {
            [BvrSoakWin]::SendMessage($no, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Write-Step "dismissed the revert-Options dialog with No"
        }
    }

    foreach ($line in ((Read-NewText $log ([ref]$logOffset)) -split "`n")) {
        if ($line -match $beatRe) { $beats++ }
    }
    # Two heartbeats, not one: a single line could be the tail of a previous
    # session's log when attaching.
    if ($beats -ge 2) { $gameplay = $true; break }

    # Past the title screen with a synthetic key, once. -Boot key presses ~15 s
    # in, by which time the menu has finished loading and can take input.
    # -Boot map gets the same press as a FALLBACK at ~60 s: if the map URL
    # routed through the front end anyway, the run is sitting at the title
    # screen and would otherwise burn the whole timeout for nothing.
    $keyAt = if ($Boot -eq "key") { 15 } else { 60 }
    if (($Boot -eq "key" -or $Boot -eq "map") -and -not $keyTried -and $i -ge $keyAt) {
        $keyTried = $true
        $gk = Join-Path $PSScriptRoot "game-key.ps1"
        if (Test-Path $gk) {
            if ($Boot -eq "map") {
                Write-Step "no heartbeat ${keyAt}s after the map boot - trying the title-screen key fallback"
            }
            Write-Step "pressing Space at the title screen (synthetic scancode)"
            & $gk -Game $Game -Key Space -Repeat 3 -NoFocus:$NoFocus | ForEach-Object { Write-Step $_ }
        } else {
            Write-Step "WARNING: -Boot $Boot but tools\game-key.ps1 does not exist"
        }
    }
}

if (-not $gameplay) {
    Write-Step "FAIL: never reached gameplay in $GameplayTimeoutSeconds s (no camera heartbeat)."
    Write-Step "The game is probably sitting at the title screen. Reach gameplay and re-run with -Attach."
    Show-Tail $log 20 "bioshockvr.log"
    exit 5
}
Write-Step "gameplay is live"

# --- baselines ---------------------------------------------------------------
# Taken AFTER gameplay, so nothing from the menu or the load counts against the run.
$traceOffset = [int64]0
if (Test-Path $trace) { $traceOffset = (Get-Item $trace).Length }
[void](Read-NewText $log ([ref]$logOffset))     # discard the pre-arm backlog
$dumpsBefore = Get-DumpCount
Write-Step ("baseline: pacetrace {0}, {1} crash dump(s)" -f `
    $(if (Test-Path $trace) { "$traceOffset bytes" } else { "ABSENT" }), $dumpsBefore)

# --- arm ---------------------------------------------------------------------
$armed = ($Arm -and $Arm.Trim() -ne "" -and $Arm.Trim() -ne "none")
if ($armed) {
    Write-Step "arming: $Arm"
    try {
        & (Join-Path $PSScriptRoot "game-batch.ps1") -Game $Game -NoFocus:$NoFocus -Delay 3 $Arm |
            ForEach-Object { Write-Step $_ }
    } catch {
        Write-Step "FAIL: could not arm '$Arm': $_"
        exit 6
    }
} else {
    Write-Step "arming nothing - vanilla soak"
}

# --- the soak ----------------------------------------------------------------
$deadline  = (Get-Date).AddMinutes($Minutes)
$started   = Get-Date
$watchdogs = @()
$lastNote  = Get-Date
$age       = 0    # last measured log age; also read by the 60 s progress note

Write-Step ("soaking for {0} min (until {1:HH:mm:ss})..." -f $Minutes, $deadline)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $elapsed = [int]((Get-Date) - $started).TotalSeconds

    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if (-not $p) {
        Write-Step "FAIL after ${elapsed}s: $proc DIED (crash, not a wedge)"
        Show-Tail $trace 40 "pacetrace.log"
        Show-Tail $log 20 "bioshockvr.log"
        exit 4
    }

    # New WATCHDOG lines are the stall detector's own verdict.
    foreach ($line in ((Read-NewText $trace ([ref]$traceOffset)) -split "`n")) {
        if ($line -match 'WATCHDOG') { $watchdogs += $line.Trim() }
    }
    if ($watchdogs.Count -gt 0) {
        Write-Step "FAIL after ${elapsed}s: WATCHDOG fired $($watchdogs.Count) time(s)"
        $watchdogs | Select-Object -First 8 | ForEach-Object { Write-Host "  $_" }
        Show-Tail $trace 40 "pacetrace.log"
        Stop-GameIfAsked
        exit 3
    }

    # The wedge signature: the log stops dead and never resumes.
    if (Test-Path $log) {
        $age = ((Get-Date) - (Get-Item $log).LastWriteTime).TotalSeconds
        if ($age -gt 20) {
            Write-Step "FAIL after ${elapsed}s: bioshockvr.log has not advanced for $([int]$age)s - WEDGED"
            Show-Tail $trace 40 "pacetrace.log"
            Show-Tail $log 20 "bioshockvr.log"
            Stop-GameIfAsked
            exit 2
        }
    }

    $dumps = Get-DumpCount
    if ($dumps -gt $dumpsBefore) {
        Write-Step "FAIL after ${elapsed}s: $($dumps - $dumpsBefore) NEW crash dump(s) in $crashDir"
        Show-Tail $log 20 "bioshockvr.log"
        Stop-GameIfAsked
        exit 8
    }

    if (((Get-Date) - $lastNote).TotalSeconds -ge 60) {
        $lastNote = Get-Date
        Write-Step ("  {0,4}s ok (log {1}s old, {2} left)" -f $elapsed, [int]$age,
                    ("{0:mm\:ss}" -f ($deadline - (Get-Date))))
    }
}

# --- verdict -----------------------------------------------------------------
# A clean result is only meaningful if the instrument was running. If pacetrace
# never appeared, the WATCHDOG check could not have failed, and this run proves
# nothing about the freeze.
if (-not (Test-Path $trace)) {
    Write-Step "INCONCLUSIVE: pacetrace.log never appeared, so the WATCHDOG check was vacuous."
    Write-Step "The trace thread starts from the Present detour - if it never started, the mod is not hooked."
    exit 7
}

Write-Step ("PASS: {0} min, {1}, no wedge, no WATCHDOG, no new dumps" -f `
    $Minutes, $(if ($armed) { "'$Arm'" } else { "vanilla" }))
exit 0
