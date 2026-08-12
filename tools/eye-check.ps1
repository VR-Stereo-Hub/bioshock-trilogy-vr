# eye-check.ps1 - the per-trial STEREO EYE CHECK (mandatory after ANY view-path
# change; sim only, no headset needed).
#
# Session 54e shipped a view-path change that passed its flat raffle acceptance
# and then broke stereo IN THE HEADSET: one world per eye, smear on head motion,
# no 3D (STATUS s54f). The flat acceptance never looked at the per-eye images -
# the stereo-only-testing rule was violated by omission. This script is that
# rule made executable: five legs, each with a PASS band, any leg out of band
# means THE TRIAL BROKE STEREO and must be reverted before continuing.
#
# The legs (bands calibrated 2026-08-12 on the reverted known-good build, this
# save; the interocular band INCLUDES quad compositing - HUD panel, aim laser -
# so it is not pure parallax. Measured that day at the fairground, s55 boot:
# interocular mean 46.5-46.8 / pct 73-75%, moved means 28.9/29.7, sep 0.0630 -
# the s54f note recorded 53-56/77% at a different spot; the band is scene-
# dependent inside [40, 70], which is why the band is that wide):
#   1. shot A: a projection layer is present and EyeSeparationM is ~0.063.
#   2. img-diff A_left vs A_right: mean ~53-56, pct-changed ~77% - two eyes of
#      ONE world. A per-eye-different WORLD reads far outside this band.
#   3. head yaw +25 deg (xrsim "head rot 25 0 0" - the FIRST arg yaws), settle,
#      shot B.
#   4. img-diff A_left vs B_left AND A_right vs B_right: BOTH large (~21-31
#      mean) - the view moved in BOTH eyes. An eye pinned to a stale or
#      unsubstituted view fails here.
#   5. img-diff B_left vs B_right: back inside the leg-2 band.
# The head pose is restored to 0 0 0 afterwards, pass or fail.
#
# Preconditions are ASSERTED, not flipped: the sim session must be running and
# FOCUSED (an unfocused capture reads QuadLayers 0 and swallows input - the
# foreground trap), and vrstereo must already be ON (this script never toggles
# game levers; a lever flip mid-trial is itself a trial contamination).
#
# Usage:
#   .\tools\eye-check.ps1                      # PASS/FAIL table + exit code
#   .\tools\eye-check.ps1 -Label vdeny-1E1367  # tag the capture folder
[CmdletBinding()]
param(
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    # Capture folder tag so trial evidence is not overwritten between trials.
    [string]$Label = "trial",
    [int]$YawDeg = 25,
    [double]$SettleSec = 3.0,
    # PASS bands. Defaults are the 2026-08-12 calibration with margin; override
    # only with a measured reason and record the recalibration in the header.
    [double]$EyeSepLo = 0.05,   [double]$EyeSepHi = 0.08,
    [double]$OcularMeanLo = 40, [double]$OcularMeanHi = 70,
    [double]$OcularPctLo = 60,  [double]$OcularPctHi = 90,
    [double]$MovedMeanMin = 12,
    [switch]$NoFocus
)

$ErrorActionPreference = 'Stop'
$shotScript = Join-Path $PSScriptRoot "xrsim-shot.ps1"
$cmdScript  = Join-Path $PSScriptRoot "xrsim-cmd.ps1"
$diffScript = Join-Path $PSScriptRoot "img-diff.ps1"
$stateScript = Join-Path $PSScriptRoot "xrsim-state.ps1"

# --- foreground the game (the unfocused-capture trap) ------------------------
Add-Type -ErrorAction SilentlyContinue @'
using System; using System.Runtime.InteropServices;
public static class BvrEyeFocus {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
'@
if (-not $NoFocus) {
    $p = @(Get-Process "BioShockInfinite" -ErrorAction SilentlyContinue |
           Where-Object { -not $_.HasExited -and $_.MainWindowHandle -ne [IntPtr]::Zero })
    $p = $p | Sort-Object Id -Descending | Select-Object -First 1
    if ($p) {
        [void][BvrEyeFocus]::ShowWindow($p.MainWindowHandle, 9)   # SW_RESTORE
        [void][BvrEyeFocus]::SetForegroundWindow($p.MainWindowHandle)
        Start-Sleep -Milliseconds 800
    }
}

$s = & $stateScript -Dir $Dir -Quiet
if (-not $s.sessionRunning) { throw "no running sim session - launch with .\tools\xrsim-launch.ps1 -Game bsi." }
if ($s.sessionState -ne "FOCUSED") {
    throw "session is $($s.sessionState), not FOCUSED - foreground the game and retry " +
          "(an unfocused capture reads empty layers by construction)."
}

# --- capture folder ----------------------------------------------------------
$tag = ($Label -replace '[^A-Za-z0-9_\-]', '_')
$outDir = Join-Path $Dir ("eyecheck\{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss"), $tag)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Parse-Diff([string]$a, [string]$b) {
    $line = @(& $diffScript -A $a -B $b)[0]
    if ($line -notmatch 'mean-abs-diff=([\d.]+)\s+max-channel-diff=(\d+)\s+pct-channels-changed\(>\d+\)=([\d.]+)%') {
        throw "img-diff output not parseable: $line"
    }
    [pscustomobject]@{ Mean = [double]$Matches[1]; Max = [int]$Matches[2]; Pct = [double]$Matches[3] }
}

$legs = New-Object System.Collections.Generic.List[object]
function Add-Leg([string]$name, [bool]$pass, [string]$measured, [string]$band) {
    $legs.Add([pscustomobject]@{ Leg = $name; Result = $(if ($pass) { "PASS" } else { "FAIL" });
                                 Measured = $measured; Band = $band }) | Out-Null
}

try {
    # Leg 1: shot A - projection layer + eye separation.
    $A = & $shotScript -Dir $Dir -Out (Join-Path $outDir "A") -Quiet
    $projOk = @($A.LayerTypes) -contains "projection"
    $sepOk  = ($A.EyeSeparationM -ge $EyeSepLo) -and ($A.EyeSeparationM -le $EyeSepHi)
    Add-Leg "1 projection+eyesep" ($projOk -and $sepOk) `
        ("layers={0} sep={1:N4}m" -f (@($A.LayerTypes) -join '+'), $A.EyeSeparationM) `
        ("projection present, sep [{0}, {1}]" -f $EyeSepLo, $EyeSepHi)

    # Leg 2: interocular diff at rest - two eyes of ONE world.
    $d2 = Parse-Diff $A.Left $A.Right
    $ok2 = ($d2.Mean -ge $OcularMeanLo) -and ($d2.Mean -le $OcularMeanHi) -and
           ($d2.Pct -ge $OcularPctLo) -and ($d2.Pct -le $OcularPctHi)
    Add-Leg "2 A_left vs A_right" $ok2 ("mean={0} pct={1}%" -f $d2.Mean, $d2.Pct) `
        ("mean [{0}, {1}], pct [{2}, {3}]" -f $OcularMeanLo, $OcularMeanHi, $OcularPctLo, $OcularPctHi)

    # Leg 3: yaw the head, settle, shot B.
    & $cmdScript -Dir $Dir -Quiet "head rot $YawDeg 0 0" | Out-Null
    Start-Sleep -Seconds $SettleSec
    $B = & $shotScript -Dir $Dir -Out (Join-Path $outDir "B") -Quiet

    # Leg 4: the view moved in BOTH eyes.
    $dL = Parse-Diff $A.Left  $B.Left
    $dR = Parse-Diff $A.Right $B.Right
    Add-Leg "4 A vs B, both eyes" (($dL.Mean -ge $MovedMeanMin) -and ($dR.Mean -ge $MovedMeanMin)) `
        ("meanL={0} meanR={1}" -f $dL.Mean, $dR.Mean) ("both >= {0}" -f $MovedMeanMin)

    # Leg 5: interocular diff after the move - still one world.
    $d5 = Parse-Diff $B.Left $B.Right
    $ok5 = ($d5.Mean -ge $OcularMeanLo) -and ($d5.Mean -le $OcularMeanHi) -and
           ($d5.Pct -ge $OcularPctLo) -and ($d5.Pct -le $OcularPctHi)
    Add-Leg "5 B_left vs B_right" $ok5 ("mean={0} pct={1}%" -f $d5.Mean, $d5.Pct) `
        ("mean [{0}, {1}], pct [{2}, {3}]" -f $OcularMeanLo, $OcularMeanHi, $OcularPctLo, $OcularPctHi)
}
finally {
    # Pass or fail, never leave the trial's head pose behind.
    try { & $cmdScript -Dir $Dir -Quiet "head rot 0 0 0" | Out-Null } catch {}
}

$legs | Format-Table -AutoSize | Out-String | Write-Host
$fail = @($legs | Where-Object { $_.Result -eq "FAIL" })
if ($fail.Count -gt 0) {
    Write-Host ("EYE CHECK: FAIL ({0} leg(s) out of band) - the trial broke stereo, revert it. Evidence: {1}" -f $fail.Count, $outDir)
    exit 1
}
Write-Host ("EYE CHECK: PASS (evidence: {0})" -f $outDir)
exit 0
