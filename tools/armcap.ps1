# armcap.ps1 - run the arm ground-truth capture (no game, no headset setup).
#
# WHAT YOU ARE DOING, physically:
#
#   1. Strap ONE controller to the elbow of the arm you are capturing, as
#      rigidly as you can. Tape, a velcro strap, an armband - whatever does not
#      let it rotate. It does not matter exactly where on the elbow it sits, but
#      it MUST NOT SHIFT during the run: the fit solves for its offset from the
#      joint once, and assumes that offset is constant.
#   2. Hold the OTHER controller in the hand of that SAME arm.
#   3. Sit down. Pull either trigger to start.
#   4. Move that arm through as many poses as you can reach, SLOWLY. Slow
#      matters more than variety - the cameras need to see the elbow, and fast
#      motion is where tracking drops.
#   5. Pull either trigger again to stop. Take the headset off to finish.
#
# COVER THE SPACE, not just the comfortable middle. The model is only as good as
# the poses it has seen, and the interesting failures are all at the edges:
#
#   - arm straight down at your side, and straight out in front
#   - hand up near your face, and above your head
#   - hand across your chest to the opposite shoulder  (the case our current
#     pole model cannot express at all)
#   - hand behind your hip, elbow forward
#   - the same positions with the wrist rolled palm-up, palm-down, thumb-in
#   - full extension, and hand tucked right in to your own shoulder (the
#     "too close" case - our solver currently overshoots there)
#
# Squeeze either grip to drop a MARKER between groups of poses. That is free and
# makes the data much easier to read afterwards - use one per group above.
#
# HAPTICS ARE THE ONLY FEEDBACK. The headset stays black on purpose:
#   1 buzz  = recording started       2 buzzes = recording stopped
#   tick every 15 s = still going     1 long buzz = shutting down
#
# THE ONE NUMBER THAT DECIDES IF THE RUN IS ANY GOOD is the tracked percentage
# printed as it goes. An elbow-mounted controller spends a lot of its life
# hidden from the headset cameras, and when it is hidden the runtime does not
# stop reporting - it dead-reckons from the IMU and drifts silently. Those rows
# are marked trk=0 and get thrown away. Aim to keep the arm forward and in view.
# Under 60% tracked and the tool says so; do it again rather than trusting it.
#
# Several short runs beat one long one - drift accumulates, and a bad run costs
# nothing if it is only two minutes.

[CmdletBinding()]
param(
    # Which physical controller is strapped to the elbow. Recorded in the CSV
    # header only; both controllers are logged raw regardless, so getting this
    # wrong is a relabel afterwards, not a lost run.
    [ValidateSet("left", "right")][string]$Elbow = "left",
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\src\Debug\armcap32.exe"

if ($Rebuild -or -not (Test-Path $exe)) {
    Write-Host "building armcap32..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
if (-not (Test-Path $exe)) { throw "armcap32.exe not found at $exe - run with -Rebuild" }

$outDir = Join-Path $env:LOCALAPPDATA "BioshockVR\armcap"
$before = @()
if (Test-Path $outDir) { $before = Get-ChildItem $outDir -Filter *.csv | Select-Object -ExpandProperty Name }

Write-Host ""
Write-Host "  elbow controller : $Elbow" -ForegroundColor Yellow
Write-Host "  hand controller  : $(if ($Elbow -eq 'left') { 'right' } else { 'left' })" -ForegroundColor Yellow
Write-Host ""
Write-Host "  EITHER TRIGGER = start/stop     EITHER GRIP = marker" -ForegroundColor Green
Write-Host "  take the headset off to finish" -ForegroundColor Green
Write-Host ""

# armcap32 is a plain 32-bit OpenXR client with no layer guard of its own, so an
# implicit x64 layer (OBS's mirror is the measured one) would fail it at instance
# creation. Opt those out for this process only, as the mod does for itself.
. (Join-Path $PSScriptRoot "lib\xr-layers.ps1")
$layerEnv = Disable-BvrUnloadableXrLayers
try {
    & $exe --elbow $Elbow
    $code = $LASTEXITCODE
} finally {
    Restore-BvrXrLayerEnv $layerEnv
}

$after = Get-ChildItem $outDir -Filter *.csv -ErrorAction SilentlyContinue
$new = $after | Where-Object { $before -notcontains $_.Name }

Write-Host ""
if ($new) {
    foreach ($f in $new) {
        $rows = (Get-Content $f.FullName | Where-Object { $_ -notmatch '^(#|sample,)' }).Count
        Write-Host ("captured: {0}  ({1:N0} rows, {2:N0} KB)" -f $f.FullName, $rows, ($f.Length / 1KB)) -ForegroundColor Cyan
    }
} elseif ($code -eq 2) {
    Write-Host "no samples recorded - was a trigger ever pulled?" -ForegroundColor Yellow
} else {
    Write-Host "no new capture file" -ForegroundColor Yellow
}

exit $code
