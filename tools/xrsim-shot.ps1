# xrsim-shot.ps1 - capture what the headset compositor would show, per eye, plus
# the JSON sidecar of numbers to assert on.
#
# This is the tool that answers questions a window screenshot cannot. game-shot
# captures the game's BACKBUFFER; XR quad layers - the aim laser, the aim dot,
# the HUD panel - exist only in the compositor and never appear there
# (docs/bioshock1/TESTING.md has recorded that limitation for this project's
# whole life). Here they are composited and counted.
#
# Returns an object; the interesting fields are Left/Right/Sbs/Json,
# Layers, LayerTypes, QuadLayers, MeanLumaL/R, NonBlackPctL/R and ClaimRatioH.
#
# Usage:
#   $s = .\tools\xrsim-shot.ps1 -Out "$env:TEMP\bvr\a"
#   .\tools\img-diff.ps1 -A $s.Left -B $s.Right      # stereo check
[CmdletBinding()]
param(
    [string]$Out = "",
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [int]$SettleFrames = 2,
    [double]$TimeoutSec = 15,
    [switch]$AllowEmpty,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$stateScript = Join-Path $PSScriptRoot "xrsim-state.ps1"
$cmdScript   = Join-Path $PSScriptRoot "xrsim-cmd.ps1"

# --- preflight, each check named after the failure it prevents ---------------
$s = & $stateScript -Dir $Dir -Quiet
if (-not $s.sessionRunning) {
    throw "no running sim session - launch with .\tools\xrsim-launch.ps1."
}
if ($s.sessionState -ne "FOCUSED" -and $s.sessionState -ne "VISIBLE") {
    throw "the session is $($s.sessionState); a capture now would be black by " +
          "construction. Wait for VISIBLE or FOCUSED."
}

# Frames must be advancing, or the capture request is never consumed and this
# would hang until the timeout with nothing to show for it. Under `pace step`
# they only advance when granted, so grant what this capture needs rather than
# making every stepped sequence interleave its own `step` calls.
$stepping = ($s.paceMode -eq "step")
if ($stepping) { & $cmdScript -Dir $Dir -Quiet "step $($SettleFrames + 1)" | Out-Null }
& $stateScript -Dir $Dir -For "frame+$SettleFrames" -TimeoutSec 10 -Quiet | Out-Null

# --- name and fire -----------------------------------------------------------
$tag = if ($Out) { [System.IO.Path]::GetFileNameWithoutExtension($Out) } else { "shot" }
$tag = ($tag -replace '[^A-Za-z0-9_\-]', '_')

$capSeqBefore = [int]$s.captureSeq
& $cmdScript -Dir $Dir -Quiet "shot $tag" | Out-Null
# The capture is consumed by a submitted frame, so step mode needs one granted.
if ($stepping) { & $cmdScript -Dir $Dir -Quiet "step 2" | Out-Null }
& $stateScript -Dir $Dir -For "captureSeq+1" -TimeoutSec $TimeoutSec -Quiet | Out-Null

$capDir = Join-Path $Dir "capture"
$jsonPath = Join-Path $capDir "$tag.json"
if (-not (Test-Path $jsonPath)) {
    throw "the sim reported a capture but $jsonPath was not written."
}
$j = Get-Content $jsonPath -Raw | ConvertFrom-Json

$left  = Join-Path $capDir "${tag}_left.png"
$right = Join-Path $capDir "${tag}_right.png"
$sbs   = Join-Path $capDir "${tag}_sbs.png"

# --- optionally copy out to a caller-chosen location -------------------------
if ($Out) {
    $outDir = Split-Path -Parent $Out
    if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    $base = if ([System.IO.Path]::GetExtension($Out)) {
        Join-Path (Split-Path -Parent $Out) ([System.IO.Path]::GetFileNameWithoutExtension($Out))
    } else { $Out }
    Copy-Item $left  "${base}_left.png"  -Force
    Copy-Item $right "${base}_right.png" -Force
    Copy-Item $sbs   "${base}_sbs.png"   -Force
    Copy-Item $jsonPath "${base}.json"   -Force
    $left = "${base}_left.png"; $right = "${base}_right.png"; $sbs = "${base}_sbs.png"; $jsonPath = "${base}.json"
}

$types = @($j.layers | ForEach-Object { $_.type })
$quads = @($j.layers | Where-Object { $_.type -eq "quad" })

if (-not $AllowEmpty -and [int]$j.layerCount -eq 0) {
    throw "the frame carried NO composition layers - the mod submitted nothing. " +
          "Is VR camera mode on? (pass -AllowEmpty to capture anyway)"
}

if (-not $Quiet) {
    Write-Host ("saved {0} + {1} (frame {2}, {3} layer(s): {4}, meanLuma {5:N1}/{6:N1}, nonBlack {7:N1}%/{8:N1}%)" -f `
        (Split-Path -Leaf $left), (Split-Path -Leaf $right), $j.frameIndex, $j.layerCount, `
        ($types -join ','), $j.stats.meanLumaL, $j.stats.meanLumaR, $j.stats.nonBlackPctL, $j.stats.nonBlackPctR)
}

[pscustomobject]@{
    Left          = $left
    Right         = $right
    Sbs           = $sbs
    Json          = $jsonPath
    Frame         = $j.frameIndex
    SessionState  = $j.sessionState
    Width         = $j.width
    Height        = $j.height
    Layers        = [int]$j.layerCount
    LayerTypes    = $types
    QuadLayers    = $quads.Count
    ProjViews     = $(if ($j.layers.Count -gt 0 -and $j.layers[0].viewCount) { [int]$j.layers[0].viewCount } else { 0 })
    MeanLumaL     = [double]$j.stats.meanLumaL
    MeanLumaR     = [double]$j.stats.meanLumaR
    NonBlackPctL  = [double]$j.stats.nonBlackPctL
    NonBlackPctR  = [double]$j.stats.nonBlackPctR
    EyeSeparationM= [double]$j.derived.eyeSeparationM
    ClaimRatioH   = [double]$j.derived.claimRatioH
    Raw           = $j
}
