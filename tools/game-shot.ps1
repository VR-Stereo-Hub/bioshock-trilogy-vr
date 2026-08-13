# Capture the BioshockHD window itself to a PNG (PrintWindow + PW_RENDERFULLCONTENT,
# which grabs D3D content on Win10/11) - not the whole desktop. Foregrounds the window
# first and waits, because the game pauses presenting while unfocused.
# Usage: .\tools\game-shot.ps1 -Out C:\path\shot.png [-Game bs2|bsi]
param(
    [Parameter(Mandatory=$true)][string]$Out,
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
public static class W {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    public struct RECT { public int L; public int T; public int R; public int B; }
}
'@
Add-Type -AssemblyName System.Drawing
switch ($Game) {
    "bs2" { $procName = "Bioshock2HD" }
    "bsi" { $procName = "BioShockInfinite" }
    default { $procName = "BioshockHD" }
}
# Live instance only - an exited process lingers while a handle is held, and
# the resulting array breaks every window call (session 38).
$p = @(Get-Process $procName -ErrorAction Stop | Where-Object {
    -not $_.HasExited -and $_.MainWindowHandle -ne [IntPtr]::Zero
} | Sort-Object Id -Descending) | Select-Object -First 1
if (-not $p) { throw "$procName is not running (no live instance with a window)" }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "no main window" }
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 2500   # unfocused game pauses presenting; let it resume
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { throw "bad window rect $w x $ht" }
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [W]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()
if (-not $ok) {
    # Fallback: copy from screen (window is foregrounded anyway)
    $bmp.Dispose()
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g2 = [System.Drawing.Graphics]::FromImage($bmp)
    $g2.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
    $g2.Dispose()
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $Out ($w x $ht, printwindow=$ok)"
