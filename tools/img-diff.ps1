# Compare two PNGs pixel-for-pixel and print how different they are, so an
# automated A/B FOV/poke sweep can decide "did the render change?" without a
# human looking. Prints mean absolute per-channel difference (0-255) and the
# max single-channel difference. A steady scene captured twice sits near 0;
# an FOV change lights it up.
#
# Session 30 additions, for "how much of the frame does a full-screen effect
# actually cover": -Grid/-Bands report WHICH REGIONS changed, not just how
# much. At a square render target a 16:9 stage fitted inside it shows up as
# unchanged top and bottom band rows with every column changed - which answers
# the coverage question with no vertex data at all. The original one-line
# output is printed first and is byte-identical, so existing callers and
# TESTING.md's noise-floor numbers are unaffected.
#
# Usage: .\tools\img-diff.ps1 -A before.png -B after.png
#        .\tools\img-diff.ps1 -A before.png -B after.png -Grid 16
#        .\tools\img-diff.ps1 -A before.png -B after.png -Bands 16
param(
    [Parameter(Mandatory=$true)][string]$A,
    [Parameter(Mandatory=$true)][string]$B,
    # n x n cells; prints an ASCII coverage map plus machine-readable cell rows.
    [int]$Grid = 0,
    # n horizontal rows AND n vertical columns, reported separately. This is the
    # one that names a letterbox or a pillarbox in a single glance.
    [int]$Bands = 0,
    # Per-channel delta above which a channel counts as "changed" (was hardcoded 8).
    [int]$Threshold = 8,
    # A cell/band counts as covered when its mean channel delta clears this.
    # Standing-still noise is ~0.4 mean, a real FOV change 4-7 (TESTING.md).
    [double]$CellMean = 1.0
)
Add-Type -AssemblyName System.Drawing

# The pixel walk moved into C#: at 2048x2048 the PowerShell loop was 16.7 M
# iterations and took longer than the screenshot it was comparing.
if (-not ("BvrImgDiff" -as [type])) {
Add-Type @'
using System;
public static class BvrImgDiff {
  // Whole-image totals, and per-cell sums for a gy x gx grid in one pass.
  public static void Diff(byte[] a, byte[] b, int w, int h, int thr,
                          int gx, int gy, long[] cellSum, long[] cellChanged,
                          out long sum, out int max, out long changed) {
    long s = 0; int m = 0; long c = 0;
    for (int y = 0; y < h; y++) {
      int cy = gy > 0 ? (int)((long)y * gy / h) : 0;
      int rowBase = y * w * 4;
      for (int x = 0; x < w; x++) {
        int i = rowBase + x * 4;
        int d0 = a[i] - b[i];     if (d0 < 0) d0 = -d0;
        int d1 = a[i+1] - b[i+1]; if (d1 < 0) d1 = -d1;
        int d2 = a[i+2] - b[i+2]; if (d2 < 0) d2 = -d2;
        int d = d0 + d1 + d2;
        s += d;
        if (d0 > m) m = d0;
        if (d1 > m) m = d1;
        if (d2 > m) m = d2;
        if (d0 > thr) c++;
        if (d1 > thr) c++;
        if (d2 > thr) c++;
        if (gx > 0 && gy > 0) {
          int cx = (int)((long)x * gx / w);
          int idx = cy * gx + cx;
          cellSum[idx] += d;
          if (d0 > thr) cellChanged[idx]++;
          if (d1 > thr) cellChanged[idx]++;
          if (d2 > thr) cellChanged[idx]++;
        }
      }
    }
    sum = s; max = m; changed = c;
  }
}
'@
}

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

# One pass computes the totals and, when asked, the finest grid requested.
# -Bands is that grid marginalised onto each axis, so both modes share the walk.
$gx = 0; $gy = 0
if ($Grid -gt 0) { $gx = $Grid; $gy = $Grid }
if ($Bands -gt 0) { if ($Bands -gt $gx) { $gx = $Bands; $gy = $Bands } }
$cells = if ($gx -gt 0) { $gx * $gy } else { 1 }
$cellSum = New-Object long[] $cells
$cellChanged = New-Object long[] $cells
$sum = [long]0; $max = 0; $changed = [long]0
[BvrImgDiff]::Diff($ba, $bb, $w, $h, $Threshold, $gx, $gy, $cellSum, $cellChanged,
                   [ref]$sum, [ref]$max, [ref]$changed)

$channels = [long]$w * $h * 3
$mean = [math]::Round($sum / $channels, 4)
$pctChanged = [math]::Round(100.0 * $changed / $channels, 2)
"mean-abs-diff=$mean  max-channel-diff=$max  pct-channels-changed(>$Threshold)=$pctChanged%  ($($w)x$($h))"

if ($gx -le 0) { return }

# Per-cell mean over the 3 channels of every pixel in the cell.
$cellMeans = New-Object double[] $cells
for ($r = 0; $r -lt $gy; $r++) {
    $y0 = [int]([long]$r * $h / $gy); $y1 = [int]([long]($r + 1) * $h / $gy)
    for ($c = 0; $c -lt $gx; $c++) {
        $x0 = [int]([long]$c * $w / $gx); $x1 = [int]([long]($c + 1) * $w / $gx)
        $px = [long](($y1 - $y0)) * ($x1 - $x0) * 3
        $idx = $r * $gx + $c
        $cellMeans[$idx] = if ($px -gt 0) { $cellSum[$idx] / $px } else { 0.0 }
    }
}

$covered = 0
$minR = $gy; $maxR = -1; $minC = $gx; $maxC = -1
for ($r = 0; $r -lt $gy; $r++) {
    for ($c = 0; $c -lt $gx; $c++) {
        if ($cellMeans[$r * $gx + $c] -ge $CellMean) {
            $covered++
            if ($r -lt $minR) { $minR = $r }; if ($r -gt $maxR) { $maxR = $r }
            if ($c -lt $minC) { $minC = $c }; if ($c -gt $maxC) { $maxC = $c }
        }
    }
}
$coverage = [math]::Round(100.0 * $covered / $cells, 1)
if ($maxR -ge 0) {
    $bx0 = [math]::Round($minC / [double]$gx, 3); $bx1 = [math]::Round(($maxC + 1) / [double]$gx, 3)
    $by0 = [math]::Round($minR / [double]$gy, 3); $by1 = [math]::Round(($maxR + 1) / [double]$gy, 3)
    $bbox = "($bx0,$by0)-($bx1,$by1)"
} else {
    $bbox = "(none)"
}
"coverage=$coverage%  covered-cells=$covered/$cells  bbox=$bbox  cell-mean-gate=$CellMean"

if ($Grid -gt 0) {
    # '#' >= 4x the gate (unambiguous), '+' over the gate, '.' below it.
    "grid $gx x $gy  ('#' strong, '+' over gate, '.' under):"
    for ($r = 0; $r -lt $gy; $r++) {
        $line = ""
        for ($c = 0; $c -lt $gx; $c++) {
            $m = $cellMeans[$r * $gx + $c]
            $line += if ($m -ge 4 * $CellMean) { "#" } elseif ($m -ge $CellMean) { "+" } else { "." }
        }
        "  $line"
    }
}

if ($Bands -gt 0) {
    # Marginals. A 16:9 stage inside a square target = the first and last rows
    # under the gate with every column over it.
    "rows (top to bottom):"
    for ($r = 0; $r -lt $gy; $r++) {
        $s = 0.0
        for ($c = 0; $c -lt $gx; $c++) { $s += $cellMeans[$r * $gx + $c] }
        $m = [math]::Round($s / $gx, 3)
        $flag = if ($m -ge $CellMean) { "CHANGED" } else { "flat   " }
        "  row {0,2}  y={1,5:0.000}-{2,5:0.000}  mean={3,8}  {4}" -f $r, ($r / [double]$gy), (($r + 1) / [double]$gy), $m, $flag
    }
    "cols (left to right):"
    for ($c = 0; $c -lt $gx; $c++) {
        $s = 0.0
        for ($r = 0; $r -lt $gy; $r++) { $s += $cellMeans[$r * $gx + $c] }
        $m = [math]::Round($s / $gy, 3)
        $flag = if ($m -ge $CellMean) { "CHANGED" } else { "flat   " }
        "  col {0,2}  x={1,5:0.000}-{2,5:0.000}  mean={3,8}  {4}" -f $c, ($c / [double]$gx), (($c + 1) / [double]$gx), $m, $flag
    }
}
