# Compare two PNGs pixel-for-pixel and print how different they are, so an
# automated A/B FOV/poke sweep can decide "did the render change?" without a
# human looking. Prints mean absolute per-channel difference (0-255) and the
# max single-channel difference. A steady scene captured twice sits near 0;
# an FOV change lights it up.
# Usage: .\tools\img-diff.ps1 -A before.png -B after.png
param(
    [Parameter(Mandatory=$true)][string]$A,
    [Parameter(Mandatory=$true)][string]$B
)
Add-Type -AssemblyName System.Drawing
$ia = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
$ib = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))
if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) {
    "SIZE MISMATCH: $($ia.Width)x$($ia.Height) vs $($ib.Width)x$($ib.Height)"
    $ia.Dispose(); $ib.Dispose()
    return
}
$w = $ia.Width; $h = $ia.Height
$rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$da = $ia.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
$db = $ib.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
$n = $w * $h * 4
$ba = New-Object byte[] $n
$bb = New-Object byte[] $n
[System.Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $n)
[System.Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $n)
$ia.UnlockBits($da); $ib.UnlockBits($db)
$ia.Dispose(); $ib.Dispose()

[long]$sum = 0
[int]$max = 0
[long]$changed = 0
# Walk RGB channels (skip alpha, index i%4 -eq 3).
for ($i = 0; $i -lt $n; $i++) {
    if (($i % 4) -eq 3) { continue }
    $d = [math]::Abs([int]$ba[$i] - [int]$bb[$i])
    $sum += $d
    if ($d -gt $max) { $max = $d }
    if ($d -gt 8) { $changed++ }
}
$channels = [long]$w * $h * 3
$mean = [math]::Round($sum / $channels, 4)
$pctChanged = [math]::Round(100.0 * $changed / $channels, 2)
"mean-abs-diff=$mean  max-channel-diff=$max  pct-channels-changed(>8)=$pctChanged%  ($($w)x$($h))"
