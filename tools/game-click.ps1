# Click at window-relative coordinates inside the BioshockHD window (foregrounds it
# first; coordinates are the same as pixel positions in a game-shot.ps1 capture).
# gameswf menus register these synthetic clicks (verified 2026-07-24: menu navigation).
# Usage: .\tools\game-click.ps1 -X 967 -Y 601 [-Game bs2|bsi]
param(
    [Parameter(Mandatory=$true)][int]$X,
    [Parameter(Mandatory=$true)][int]$Y,
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [switch]$Force
)
# One game owns the headset at a time (see tools\lib\assert-no-conflict.ps1).
if ($Game -eq "bsi") {
    . (Join-Path $PSScriptRoot "lib\assert-no-conflict.ps1")
    Assert-NoConflictingGame -Game $Game -Force:$Force
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class M {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public struct RECT { public int L; public int T; public int R; public int B; }
}
'@
switch ($Game) {
    "bs2" { $procName = "Bioshock2HD" }
    "bsi" { $procName = "BioShockInfinite" }
    default { $procName = "BioshockHD" }
}
$p = Get-Process $procName -ErrorAction Stop
$h = $p.MainWindowHandle
[M]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
$r = New-Object M+RECT
[M]::GetWindowRect($h, [ref]$r) | Out-Null
$sx = $r.L + $X; $sy = $r.T + $Y
[M]::SetCursorPos($sx, $sy) | Out-Null
Start-Sleep -Milliseconds 400   # let gameswf register the hover
[M]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
Start-Sleep -Milliseconds 80
[M]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
"clicked window($X,$Y) = screen($sx,$sy)"
