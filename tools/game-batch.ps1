# game-batch.ps1 - run a SEQUENCE of seam commands with delays, foregrounding
# the game ONCE and never taking a screenshot.
#
# Why this exists (session 33): the documented pattern "pair every game-cmd
# with a game-shot" is there because the game pauses while unfocused and the
# 1 Hz command poll then takes forever to tick. But game-shot's PrintWindow
# does a full re-render, and a loop of them WEDGED BioShock 2 (Responding=False,
# no crash dump, force-kill required) part way through a 6-candidate poke hunt.
# Focus is what the poll needs - the re-render is not. This asserts focus and
# then just writes the file, which is enough: with the window focused, BS2's
# ProcessEvent traffic is high enough that the poll gate ticks within ~1 s.
#
# Read results from the LOG (tools/tail-log.ps1 -Game bs2), not from here.
#
# Usage:
#   .\tools\game-batch.ps1 -Game bs2 -Delay 2.5 "fgfov addr 48410488" "fgfov on" "fovaudit"
#   .\tools\game-batch.ps1 -Game bs2 "recenter"
#
# Each argument is ONE poll cycle (one write of command.txt, then -Delay
# seconds). Group commands that must land together by separating them with a
# semicolon: "fgfov addr X; fgfov on".
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1",
    [double]$Delay = 2.5,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps
)

$ErrorActionPreference = 'Stop'
if (-not $Steps -or $Steps.Count -eq 0) { throw "no steps given" }

$proc = if ($Game -eq "bs2") { "Bioshock2HD" } else { "BioshockHD" }
$file = if ($Game -eq "bs2") { "$env:LOCALAPPDATA\BioshockVR\bs2\command.txt" }
        else { "$env:LOCALAPPDATA\BioshockVR\command.txt" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class BvrFg {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
"@ -ErrorAction SilentlyContinue

function Assert-Focus {
    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if (-not $p) { throw "$proc is not running" }
    if (-not $p.Responding) { throw "$proc is NOT RESPONDING - recover before batching" }
    [void][BvrFg]::ShowWindow($p.MainWindowHandle, 9)  # SW_RESTORE
    [void][BvrFg]::SetForegroundWindow($p.MainWindowHandle)
}

Assert-Focus
Start-Sleep -Milliseconds 800   # let the engine resume presenting before step 1

$i = 0
foreach ($step in $Steps) {
    $i++
    $lines = ($step -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    Assert-Focus
    for ($try = 0; $try -lt 30; $try++) {
        try {
            [System.IO.File]::WriteAllText($file, (($lines -join "`n") + "`n"))
            break
        } catch { Start-Sleep -Milliseconds 200 }   # 1 Hz poller holds a share lock
    }
    Write-Output ("[{0}/{1}] {2}" -f $i, $Steps.Count, ($lines -join ' ; '))
    Start-Sleep -Seconds $Delay
}
Write-Output "batch done - read results from the log"
