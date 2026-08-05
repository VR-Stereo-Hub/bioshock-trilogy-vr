# Write one or more commands to the mod's command.txt seam, retrying past the
# transient share-lock when the game's 1 Hz poller has the file open.
# Foregrounds the game first: on bs1/bs2 the poller runs inside the CalcView
# hook, which the engine pauses while the window is unfocused, so an unfocused
# game never reads the file. On bsi the poller is core and ticks from Present
# (session 35), which does not stop on focus loss - foregrounding is kept anyway
# because it costs nothing and keeps the three games' flows identical.
# NOTE for bsi: a command.txt that already exists when the game starts is
# SKIPPED, not run. Write it again once the game is up.
# Usage: .\tools\game-cmd.ps1 "memscani 123" "memlist"
#        .\tools\game-cmd.ps1 -Game bs2 "recenter"
#        .\tools\game-cmd.ps1 -Game bsi "recenter"
# PositionalBinding=$false so -Game can never swallow a command: as a positional
# parameter it did, and every bare `game-cmd.ps1 "vrinput on"` call failed
# validation, which silently broke boot.ps1's menu-press loop.
[CmdletBinding(PositionalBinding=$false)]
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [switch]$Force,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$Lines
)
# One game owns the headset at a time (see tools\lib\assert-no-conflict.ps1).
if ($Game -eq "bsi") {
    . (Join-Path $PSScriptRoot "lib\assert-no-conflict.ps1")
    Assert-NoConflictingGame -Game $Game -Force:$Force
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Cmd { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
'@
switch ($Game) {
    "bs2" { $procName = "Bioshock2HD" }
    "bsi" { $procName = "BioShockInfinite" }
    default { $procName = "BioshockHD" }
}
$p = @(Get-Process $procName -ErrorAction SilentlyContinue | Where-Object {
    -not $_.HasExited -and $_.MainWindowHandle -ne [IntPtr]::Zero
} | Sort-Object Id -Descending) | Select-Object -First 1
if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
    [Cmd]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 400
}
switch ($Game) {
    "bs2" { $cmd = "$env:LOCALAPPDATA\BioshockVR\bs2\command.txt" }
    "bsi" { $cmd = "$env:LOCALAPPDATA\BioshockVR\bsi\command.txt" }
    default { $cmd = "$env:LOCALAPPDATA\BioshockVR\command.txt" }
}
$cmdDir = Split-Path -Parent $cmd
if (-not (Test-Path $cmdDir)) { New-Item -ItemType Directory -Force $cmdDir | Out-Null }
$text = ($Lines -join "`n")
for ($i = 0; $i -lt 30; $i++) {
    try {
        [System.IO.File]::WriteAllText($cmd, $text + "`n")
        "wrote $($Lines.Count) command(s)"
        return
    } catch {
        Start-Sleep -Milliseconds 200
    }
}
throw "could not write command.txt after retries"
