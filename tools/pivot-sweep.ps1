# pivot-sweep.ps1 - does the hand stay on the controller whichever way the controller
# points? The simulator holds the right controller's GRIP at ONE point and turns it through
# a set of orientations; at each one the mod's PIVOTPROBE line (bone-level: the written palm
# read back from the engine's array, taken to world, against the grip point) is read, and a
# per-eye compositor capture is taken for the render-level check. Numeric oracle: the
# largest PIVOTPROBE error over the sweep must be under -MaxCm (default 1.0).
#
# docs/bioshock1/PIVOT_SIM_PROTOCOL.md says how to read the result and what to do next.
#
#   .\tools\pivot-sweep.ps1                       # game already in gameplay on the sim (boot.ps1 -Attach)
#   .\tools\pivot-sweep.ps1 -Launch               # xrsim-launch + boot -Attach first
#   .\tools\pivot-sweep.ps1 -Hand l               # the left hand
#
# Exit 0 = pass, 1 = the hand left the controller somewhere (the table says where),
# 2 = no PIVOTPROBE lines (the probe or the drive is not running).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [switch]$Launch,
    [ValidateSet("l", "r")][string]$Hand = "r",
    [double]$MaxCm = 1.0,
    [string]$Out = "$env:TEMP\bvr\pivot",
    [switch]$NoShots,
    # The grip point, XR metres (x right, y up, -z forward). The sim's head stands at 1.6 m.
    # s87: the first default (0.20 1.20 -0.40) sat 45 deg below the view, the hand half off
    # the bottom of every capture; this one is ~18 deg down and ~12 deg right, in full view.
    [string]$Point = "0.10 1.45 -0.45",
    # s89: add yaw +-90 / +-120 and pitch +-70 (the "90 deg one way desyncs" report).
    [switch]$Wide,
    # s89: a short, dense laser from the grip origin (the sim's aim pose IS its grip pose), so every
    # capture shows the grip's -Z axis - it should run through the drawn fist along its handle.
    [switch]$Laser
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if ($Launch) {
    & (Join-Path $PSScriptRoot "xrsim-launch.ps1") -Game bs1 | Out-Null
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "boot.ps1") -Attach
    if ($LASTEXITCODE -ne 0) { throw "boot.ps1 did not reach gameplay" }
}

# The grip held at one point (-Point), the head level and forward.
# (rotation yaw pitch roll, degrees)
$p = $Point
$orients = @(
    @{ name = "forward";    rot = "0 0 0" },
    @{ name = "yaw-30";     rot = "-30 0 0" },
    @{ name = "yaw+30";     rot = "30 0 0" },
    @{ name = "yaw-60";     rot = "-60 0 0" },
    @{ name = "yaw+60";     rot = "60 0 0" },
    @{ name = "pitch-45";   rot = "0 -45 0" },
    @{ name = "pitch+45";   rot = "0 45 0" },
    @{ name = "roll-90";    rot = "0 0 -90" },
    @{ name = "roll+90";    rot = "0 0 90" },
    @{ name = "yaw+45p-30"; rot = "45 -30 0" },
    @{ name = "yaw-45p+30"; rot = "-45 30 0" }
)
if ($Wide) {
    $orients += @(
        @{ name = "yaw-90";   rot = "-90 0 0" },
        @{ name = "yaw+90";   rot = "90 0 0" },
        @{ name = "yaw-120";  rot = "-120 0 0" },
        @{ name = "yaw+120";  rot = "120 0 0" },
        @{ name = "pitch-70"; rot = "0 -70 0" },
        @{ name = "pitch+70"; rot = "0 70 0" },
        @{ name = "yaw-90r90"; rot = "-90 0 90" },
        @{ name = "yaw+90r-90"; rot = "90 0 -90" }
    )
}

# ONE write: game-cmd replaces command.txt, and the game polls it at 1 Hz, so two calls in a
# row lose the first (s87: the probe never armed when the laser line followed it).
# The laser puts the controller's ray in every capture (PIVOT_SIM_PROTOCOL.md).
$laserCmds = @("vraim laser on")
if ($Laser) { $laserCmds += "vraim laser 24 0.0 0.30 0.35" }   # 24 dots over the first 30 cm
& (Join-Path $PSScriptRoot "game-cmd.ps1") (@("vrhands pivotprobe on") + $laserCmds) | Out-Null
Start-Sleep -Milliseconds 1500
& (Join-Path $PSScriptRoot "xrsim-cmd.ps1") "reset" "head rot 0 0 0" | Out-Null
$rows = @()
$tag = $Hand.ToUpper()
foreach ($o in $orients) {
    & (Join-Path $PSScriptRoot "xrsim-cmd.ps1") "hand $Hand grip pose $p $($o.rot)" "hand $Hand aim pose $p $($o.rot)" | Out-Null
    # Let the drive and the probe settle: the probe prints at 5 Hz, the drive every frame.
    & (Join-Path $PSScriptRoot "xrsim-state.ps1") -For "frame+90" -Quiet | Out-Null
    Start-Sleep -Milliseconds 600
    $line = Get-Content $log -Tail 400 | Where-Object { $_ -match "PIVOTPROBE $tag\b" } | Select-Object -Last 1
    $err = $null; $off = $null
    if ($line -match "error ([0-9.]+) cm \(fwd ([-0-9.]+) right ([-0-9.]+) up ([-0-9.]+)\) \| hand ([0-9.]+) deg") {
        $err = [double]$Matches[1]; $off = [double]$Matches[5]
        $fwd = [double]$Matches[2]; $right = [double]$Matches[3]; $up = [double]$Matches[4]
    }
    $shot = $null
    if (-not $NoShots) {
        try { $shot = & (Join-Path $PSScriptRoot "xrsim-shot.ps1") -Out (Join-Path $Out "$Hand-$($o.name)") -Quiet } catch { $shot = $null }
    }
    $rows += [pscustomobject]@{
        orientation = $o.name; rot = $o.rot; errorCm = $err; fwdCm = $fwd; rightCm = $right; upCm = $up
        handOffViewDeg = $off; capture = if ($shot) { $shot.Left } else { "" }; line = $line
    }
}
& (Join-Path $PSScriptRoot "game-cmd.ps1") "vrhands pivotprobe off" | Out-Null

$rows | Format-Table orientation, rot, errorCm, fwdCm, rightCm, upCm, handOffViewDeg -AutoSize | Out-String | Write-Output
$rows | ConvertTo-Json -Depth 3 | Set-Content -Encoding ASCII (Join-Path $Out "pivot-sweep.json")
$measured = @($rows | Where-Object { $_.errorCm -ne $null })
if ($measured.Count -eq 0) { Write-Output "PIVOT SWEEP: no PIVOTPROBE lines - is the drive on and the probe armed?"; exit 2 }
$worst = ($measured | Measure-Object -Property errorCm -Maximum).Maximum
$at = ($measured | Sort-Object errorCm -Descending | Select-Object -First 1).orientation
Write-Output ("PIVOT SWEEP: {0}/{1} orientations measured, worst palm-vs-grip error {2:N2} cm at {3} (limit {4} cm) -> {5}" -f `
    $measured.Count, $rows.Count, $worst, $at, $MaxCm, $(if ($worst -le $MaxCm) { "PASS" } else { "FAIL" }))
Write-Output "captures and pivot-sweep.json in $Out"
if ($worst -le $MaxCm) { exit 0 } else { exit 1 }
