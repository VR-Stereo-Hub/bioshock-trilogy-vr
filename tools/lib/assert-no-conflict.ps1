# Refuses to drive one BioShock while another one is running.
#
# Only ONE game can own the headset at a time, and BioShock 2 development runs in
# parallel with BioShock Infinite development (user directive, 2026-07-31 session 34).
# Starting or driving a second game mid-test steals the headset from whoever is
# already using it and invalidates their run.
#
# This guards RUNTIME contention only. Building, installing, uninstalling and
# packaging touch files on disk, never the headset, so those scripts do NOT call
# this and must keep working while any game runs.
#
# Currently wired into the -Game bsi paths only. The check itself is general, so
# the BS1/BS2 scripts can adopt it later without a rewrite; it is deliberately not
# wired into them yet so this branch cannot disturb a parallel BS2 session.
#
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).

# Process name (no .exe) for each game we know about.
$script:BvrGameProcess = @{
    "bs1" = "BioshockHD"
    "bs2" = "Bioshock2HD"
    "bsi" = "BioShockInfinite"
}

function Assert-NoConflictingGame {
    <#
    .SYNOPSIS
    Throws if a BioShock other than $Game is currently running.

    .PARAMETER Game
    The game this script is about to drive: bs1, bs2 or bsi.

    .PARAMETER Force
    Skip the check. For the rare case of deliberately comparing two games side by
    side on the desktop, with no headset involved.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Game,
        [switch]$Force
    )

    if ($Force) { return }

    $conflicts = @()
    foreach ($key in $script:BvrGameProcess.Keys) {
        if ($key -eq $Game) { continue }
        $name = $script:BvrGameProcess[$key]
        $proc = Get-Process $name -ErrorAction SilentlyContinue
        if ($proc) { $conflicts += "$name (pid $($proc[0].Id), -Game $key)" }
    }

    if ($conflicts.Count -gt 0) {
        $msg = @"
Refusing to drive '$Game': another BioShock is already running.

  running: $($conflicts -join "`n           ")

Only one game can own the headset at a time. Close the other game and retry, or
postpone this test. Building and installing are NOT blocked by this - only the
scripts that launch or drive a running game are.

Pass -Force to override (desktop-only comparisons, no headset).
"@
        throw $msg
    }
}
