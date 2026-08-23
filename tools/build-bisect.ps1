# Build several revisions side by side so a perceptual symptom can be localised
# to a SESSION in one headset sitting, instead of one rebuild per question.
#
# WHY THIS EXISTS. Session 64 spent four rounds guessing at a "distant geometry
# goes fuzzy while walking" report - each guess a plausible mechanism, each one
# wrong, each one costing a headset run. Reasoning from code to a perceptual
# symptom does not converge. Bisecting does, and the only thing that made it
# expensive was that swapping revisions meant asking for a rebuild every time.
#
# Each revision is built in its own git WORKTREE, so your working tree is never
# checked out, stashed or otherwise touched. Uncommitted work is safe.
#
#   .\tools\build-bisect.ps1                       # main, s63 tip, and HEAD
#   .\tools\build-bisect.ps1 -Revs main,08785f8    # pick your own
#   .\tools\build-bisect.ps1 -Release
#
# Output: build\bisect\<label>\bioshockvr.dll plus a README naming the swap.

[CmdletBinding()]
param(
    [string[]] $Revs = @('main', 's63-bs1-comfort', 'HEAD'),
    [switch] $Release
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$config = if ($Release) { 'Release' } else { 'Debug' }
$outRoot = Join-Path $repo 'build\bisect'
$wtRoot = Join-Path $env:TEMP 'bvr-bisect-worktrees'

Write-Host ''
Write-Host '  BioShock VR - bisect build' -ForegroundColor Cyan
Write-Host '  --------------------------'
Write-Host "  Config:    $config"
Write-Host "  Revisions: $($Revs -join ', ')"
Write-Host "  Output:    $outRoot"
Write-Host ''

New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
New-Item -ItemType Directory -Force -Path $wtRoot | Out-Null

$built = @()

foreach ($rev in $Revs) {
    # A stable, filesystem-safe label, plus the short sha so a moved branch is
    # never mistaken for the revision it used to point at.
    $sha = (git -C $repo rev-parse --short $rev).Trim()
    if (-not $?) { throw "Cannot resolve revision '$rev'" }
    $label = ($rev -replace '[^A-Za-z0-9._-]', '_') + "-$sha"
    $outDir = Join-Path $outRoot $label
    $dest = Join-Path $outDir 'bioshockvr.dll'

    if (Test-Path $dest) {
        Write-Host "  [$label] already built, skipping" -ForegroundColor DarkGray
        $built += [pscustomobject]@{ Label = $label; Rev = $rev; Sha = $sha; Path = $dest }
        continue
    }

    Write-Host "  [$label] building..." -ForegroundColor Yellow
    $wt = Join-Path $wtRoot $label

    if (-not (Test-Path $wt)) {
        # --detach: never move a branch ref out from under the working tree.
        git -C $repo worktree add --detach $wt $sha | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed for $rev" }
    }

    # Submodule contents live in .git/modules and are shared, so this is cheap -
    # but a worktree does NOT get them populated automatically and the build
    # fails on a missing MinHook.h without it.
    Push-Location $wt
    try {
        git submodule update --init --recursive | Out-Null
        if ($Release) { & (Join-Path $wt 'tools\build.ps1') -Release }
        else { & (Join-Path $wt 'tools\build.ps1') }
        if ($LASTEXITCODE -ne 0) { throw "build failed for $rev" }
    } finally {
        Pop-Location
    }

    $src = Join-Path $wt "build\src\$config\bioshockvr.dll"
    if (-not (Test-Path $src)) { throw "no DLL produced for $rev at $src" }

    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    Copy-Item $src $dest -Force
    # The subject line, so a folder is identifiable months later without git.
    (git -C $repo log -1 --format='%h %ad %s' --date=short $sha) |
        Set-Content -Path (Join-Path $outDir 'COMMIT.txt') -Encoding utf8

    Write-Host "  [$label] -> $dest" -ForegroundColor Green
    $built += [pscustomobject]@{ Label = $label; Rev = $rev; Sha = $sha; Path = $dest }
}

# ---- the swap instructions, written next to the builds ---------------------
$gamePath = 'the game folder (Build\Final, beside BioshockHD.exe)'
$readme = @"
BioShock VR - bisect builds
===========================

One DLL per revision. Swap ONE FILE to change revision - nothing else moves.

  copy /y "<folder>\bioshockvr.dll" "<game>\bioshockvr.dll"

where <game> is $gamePath.

ONLY bioshockvr.dll changes between these. xinput1_3.dll, bvr_steamvr32.dll and
openvr_api.dll are already installed and are not revision-specific for this
comparison, so leave them alone.

THE GAME MUST BE CLOSED before copying, and CHECK THE BUILD STAMP after every
swap. A stale DLL has invalidated whole sessions of this project:

  findstr /c:"build:" "%LOCALAPPDATA%\BioshockVR\bioshockvr.log"

The short sha in that line must match the folder you just copied from.

Revisions in this set
---------------------
$($built | ForEach-Object { "  $($_.Label)`n    $(Get-Content (Join-Path (Split-Path $_.Path) 'COMMIT.txt') -ErrorAction SilentlyContinue)" } | Out-String)

How to run the comparison
-------------------------
Use the SAME corridor and the SAME walk for each one, and change nothing else.
Perceptual symptoms are easy to talk yourself into, so a fixed route matters more
than a long one.

  1. Swap the DLL, confirm the build stamp in the log.
  2. Walk the route. Look at detail 10-15 feet out WHILE MOVING.
  3. Stop. Look at the same detail standing still.
  4. Write down better / same / worse - nothing more.

Two further splits, both free, on whichever revision shows it:

  vrcam headbob off     s63 replaces the camera origin with Pawn.Location +
                        eyeHeight, which advances at the game's TICK rate rather
                        than the render rate. If the blur goes, that is it.
  vrstereo off          mono. If the blur goes, it is an eye-pairing problem
                        (the two eyes separated in TIME), not a camera one.

Also worth separating: walk in a straight line with your head still, then stand
and rotate on the spot. Translation and rotation artefacts have different causes,
and knowing which one smears halves the search.
"@

$readme | Set-Content -Path (Join-Path $outRoot 'README.txt') -Encoding utf8

Write-Host ''
Write-Host "  Done. $($built.Count) revision(s) in $outRoot" -ForegroundColor Cyan
Write-Host "  Read $outRoot\README.txt for the swap and the test route."
Write-Host ''
Write-Host '  Worktrees are kept for fast rebuilds. To reclaim the space:' -ForegroundColor DarkGray
Write-Host "    git worktree remove --force <path>   (see: git worktree list)" -ForegroundColor DarkGray
Write-Host ''
