# arm-sweep.ps1 - the IK arms on the skin palette (Dishonored arm_ik_draw.inc, s88) through the
# reach envelope, both hands mirrored: neutral, full forward reach, close to the chest, out to
# the side, raised, lowered, across the body. Game in gameplay on the sim.
#
#   .\tools\arm-sweep.ps1 [-Out <dir>]
#
# Per position: a compositor capture and the HANDS line (reach used per side, the solved
# upper-arm heads, the shoulder slide). Read the captures for a cuff gap, a broken elbow or an
# arm through the body; read `reach used` for the solver's envelope (over ~100% the shoulder
# slides, Dishonored's form).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([string]$Out = "$env:TEMP\bvr\arms")
$ErrorActionPreference = 'Stop'
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$sim = Join-Path $PSScriptRoot "xrsim-cmd.ps1"
$shot = Join-Path $PSScriptRoot "xrsim-shot.ps1"
# right hand x y z (metres, XR) and yaw pitch roll; the left is mirrored (x and yaw, roll negated)
$poses = @(
    @{ name = "neutral";  p = @(0.18, 1.40, -0.42); r = @(0, 0, 0) },
    @{ name = "reach";    p = @(0.22, 1.45, -0.72); r = @(0, 0, 0) },
    @{ name = "chest";    p = @(0.12, 1.32, -0.20); r = @(20, 20, 0) },
    @{ name = "side";     p = @(0.55, 1.38, -0.25); r = @(-60, 0, 0) },
    @{ name = "raised";   p = @(0.20, 1.78, -0.40); r = @(0, 40, 0) },
    @{ name = "lowered";  p = @(0.25, 1.02, -0.22); r = @(0, -40, 0) },
    @{ name = "across";   p = @(-0.12, 1.40, -0.38); r = @(40, 0, 0) }
)
& $sim "reset" "head rot 0 0 0" | Out-Null
foreach ($q in $poses) {
    $rp = "{0} {1} {2} {3} {4} {5}" -f $q.p[0], $q.p[1], $q.p[2], $q.r[0], $q.r[1], $q.r[2]
    $lp = "{0} {1} {2} {3} {4} {5}" -f (-$q.p[0]), $q.p[1], $q.p[2], (-$q.r[0]), $q.r[1], (-$q.r[2])
    & $sim "hand r grip pose $rp" "hand r aim pose $rp" "hand l grip pose $lp" "hand l aim pose $lp" | Out-Null
    Start-Sleep -Milliseconds 2300   # one HANDS line (2 s) lands inside the hold
    & $shot -Out (Join-Path $Out $q.name) -Quiet | Out-Null
    $line = Get-Content $log -Tail 300 | Select-String "\[bones\] HANDS: \d" | Select-Object -Last 1
    $reach = if ($line -and $line.Line -match "reach used L (\d+)% R (\d+)%, shoulder slid up to ([0-9.]+) UU") {
        "reach L $($Matches[1])% R $($Matches[2])%, slide $($Matches[3]) UU" } else { "no HANDS line" }
    "{0,-9} {1}" -f $q.name, $reach
}
"captures in $Out (<name>_left.png)"
