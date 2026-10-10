# sim-load-save.ps1 - from the MAIN MENU of a simulator run, load one entry of the Load Game
# list by its row (0 = newest), NEVER touching Continue. The tester's rule (2026-10-08):
# Continue lands on whatever save is newest, which has been a hands-less scripted sequence;
# the hand work needs the save stamped Aug 03 2026 11:01:57 PM (Medical Pavilion, wrench up),
# row 10 on that day's list. Row numbers move when saves are added - check a capture.
#
#   .\tools\xrsim-launch.ps1 -Game bs1           # the game, on the sim, at the intro
#   .\tools\sim-load-save.ps1                     # waits for the main menu, loads row 10
#   .\tools\sim-load-save.ps1 -Row 3 -FromPause   # from gameplay: pause menu > Load > row 3
#
# Drives the menus with `vrinput test press` (the mod's synthetic pad) and reads the mod's
# log for the screen transitions. Exit 0 on "view state: GAMEPLAY" after the load.
param(
    [int]$Row = 10,
    [switch]$FromPause,
    [int]$MenuWaitSec = 60
)
$ErrorActionPreference = 'Stop'
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
# Every press waits for the game's own "input: test press" echo before the next one.
# game-cmd REPLACES command.txt and the game polls it from CalcView, which does not run
# during the intro movies: the first version of this script wrote nine presses into the
# intro, eight were overwritten unread, and the one that survived pressed Continue.
function PressCount { @(Get-Content $log -Tail 2000 | Select-String "input: test press").Count }
function Press($b, $ms = 150) {
    for ($try = 0; $try -lt 3; $try++) {
        $before = PressCount
        & (Join-Path $PSScriptRoot "game-cmd.ps1") "vrinput test press $b $ms" | Out-Null
        $until = (Get-Date).AddSeconds(6)
        while ((Get-Date) -lt $until) {
            Start-Sleep -Milliseconds 250
            if ((PressCount) -gt $before) { Start-Sleep -Milliseconds 700; return }
        }
    }
    throw "the game never consumed 'press $b' (is the window focused and the mod polling?)"
}
function TopScreen { (Get-Content $log -Tail 20000 | Select-String "screens: top = " | Select-Object -Last 1) -replace '.*top = "([^"]+)".*', '$1' }

if (-not (Get-Process BioshockHD -ErrorAction SilentlyContinue)) { "FAIL: BioshockHD is not running"; exit 1 }
& (Join-Path $PSScriptRoot "game-cmd.ps1") "vrinput on" | Out-Null
Start-Sleep -Seconds 2

if ($FromPause) {
    Press "START" 250; Start-Sleep -Seconds 3        # Resume / Save / Load / ...
    Press "DD"
    Press "DD"         # Load
} else {
    # The intro movies end on the main menu (Continue highlighted). Wait for it rather than
    # pressing through: an A press here is a Continue press.
    $deadline = (Get-Date).AddSeconds($MenuWaitSec)
    # The CURRENT top screen must be the menu movie AND the view must be the menu: a
    # PausePC line alone has matched before the intro finished.
    function InMenu {
        $v = Get-Content $log -Tail 20000 | Select-String "view state: " | Select-Object -Last 1
        ((TopScreen) -like "*PausePC*") -and $v -and ($v.Line -like "*menu/cutscene*")
    }
    while ((Get-Date) -lt $deadline -and -not (InMenu)) { Start-Sleep -Seconds 2 }
    if (-not (InMenu)) { "FAIL: main menu not detected (top screen '$(TopScreen)')"; exit 1 }
    Start-Sleep -Seconds 2
    Press "DD"
    Press "DD"         # Continue > New Game > Load Game
}
Press "A"; Start-Sleep -Seconds 3                     # the list, newest row highlighted
for ($i = 0; $i -lt $Row; $i++) { Press "DD" }
Start-Sleep -Seconds 1
Press "A"; Start-Sleep -Seconds 3                     # "Load saved game?"
$marker = (Get-Date).ToString("HH:mm:ss")             # BEFORE Yes: a small save is in play
Press "A"                                             # Yes   before the press even returns
"selected row $Row, loading..."
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $hit = Get-Content $log -Tail 300 | Select-String "view state: GAMEPLAY" | Select-Object -Last 1
    if ($hit -and ($hit.Line -replace '^\[(\d\d:\d\d:\d\d).*', '$1') -ge $marker) { "GAMEPLAY: $($hit.Line)"; exit 0 }
}
"FAIL: gameplay not reached after the load"; exit 1
