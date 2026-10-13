# handback-test.ps1 - does a melee attack hand the weapon hand to the game's swing clip and give
# it back (Dishonored's hand-back, src/game/bioshock1r/handback.h)? Game in gameplay on the sim,
# wrench out (tools\sim-load-save.ps1 -Row 10 lands there).
#
#   .\tools\handback-test.ps1                  # a TRIGGER attack: the right hand goes to the clip
#   .\tools\handback-test.ps1 -Swing           # a PHYSICAL swing (vrinput swing sim): stays tracked
#   .\tools\handback-test.ps1 -Force both      # the blend alone, no animation (vrhands handback force)
#
# Holds both controllers still in view, fires, takes -Shots compositor captures as fast as the
# sim allows, then prints the [handback] lines of the run: the attack's verdict, the state
# transitions, the weight ends and the TRACE (drawn palm vs the controller and the game's palm).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [switch]$Swing,
    [ValidateSet("", "l", "r", "both")][string]$Force = "",
    [int]$Shots = 4,
    [string]$Out = "$env:TEMP\bvr\handback"
)
$ErrorActionPreference = 'Stop'
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$sim = Join-Path $PSScriptRoot "xrsim-cmd.ps1"
$cmd = Join-Path $PSScriptRoot "game-cmd.ps1"
$shot = Join-Path $PSScriptRoot "xrsim-shot.ps1"

$mark = (Get-Content $log).Count
& $sim "reset" "head rot 0 0 0" "hand r grip pose 0.18 1.40 -0.42 0 0 0" "hand r aim pose 0.18 1.40 -0.42 0 0 0" `
    "hand l grip pose -0.18 1.40 -0.42 0 0 0" "hand l aim pose -0.18 1.40 -0.42 0 0 0" | Out-Null
Start-Sleep -Seconds 2
& $shot -Out (Join-Path $Out "before") -SettleFrames 1 -Quiet | Out-Null

if ($Force) {
    & $cmd "vrhands handback force $Force" | Out-Null
    Start-Sleep -Milliseconds 1200
    & $shot -Out (Join-Path $Out "forced") -SettleFrames 1 -Quiet | Out-Null
    & $cmd "vrhands handback force off" | Out-Null
} elseif ($Swing) {
    # Speed humps through the real detector: the gesture fires RT itself (core swing module).
    & $cmd "vrinput swing on" "vrinput swing sim 4.0 180 1" | Out-Null
} else {
    & $sim "trigger r 1.0" | Out-Null
    Start-Sleep -Milliseconds 120
    & $sim "trigger r 0" | Out-Null
}
for ($i = 0; $i -lt $Shots; $i++) { & $shot -Out (Join-Path $Out "during$i") -SettleFrames 1 -Quiet | Out-Null }
Start-Sleep -Seconds 2
& $shot -Out (Join-Path $Out "after") -SettleFrames 1 -Quiet | Out-Null

Get-Content $log | Select-Object -Skip $mark | Select-String "\[handback\] (melee attack|hands state|right hand|left hand|TRACE)|\[swing\]" |
    ForEach-Object { $_.Line }
"captures in $Out (before, during0..$($Shots - 1), after; *_left.png)"
