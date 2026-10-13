# afw-replay.ps1 - replay the alternate frame warp's held-eye rebuild (src/game/bioshock1r/afw_gpu.cpp)
# on a capture written by `afw dump` / the F10 "AFW capture" button, with any lever changed; write
# viewable BMPs of every input and output; print the hands' depth and the cameras; score each rebuild
# against the next native frame of that eye. Never launches the game. Ported from the Dishonored VR
# mod's tools/afw-replay.ps1. The capture and this output are game output: local only, never committed.
#
#   .\tools\afw-replay.ps1 -Capture "$env:LOCALAPPDATA\BioshockVR\dumps\afw-20261010-221500"
#   .\tools\afw-replay.ps1 -Capture <dir> -Mask 0            # the old code: the held eye's hands carried as world
#   .\tools\afw-replay.ps1 -Capture <dir> -Gain 1.12 -Tint   # a hand depth gain, sources painted
#   .\tools\afw-replay.ps1 -Capture <dir> -NoHeld            # fresh eye only (a control)
#   .\tools\afw-replay.ps1 -Capture <dir> -FgTan 1.38        # the hands drawn with their own lens (a tangent ratio)
# The disparity probe compares measured/predicted disparity; the world is the control (1.0).
# A foreground ratio alone is not a depth/lens calibration: motion or eye-dependent IK can bias it.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [Parameter(Mandatory = $true)][string]$Capture,
    [string]$Out = "",
    [int]$Mask = -1,
    [double]$Gain = 0,
    [int]$Stale = -1,
    [int]$Grid = 0,
    [switch]$Tint,
    [switch]$NoHeld,
    [switch]$NoFresh,
    [switch]$NoPictures,
    [switch]$Delete, # remove this capture folder only after every present replays successfully
    [double]$FgTan = 0,
    [int]$Refine = -1
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Capture)) { throw "capture folder not found: $Capture" }
# Resolve before Push-Location; -Delete must operate on the explicitly named directory.
$Capture = (Resolve-Path -LiteralPath $Capture).ProviderPath
if (-not (Get-Item -LiteralPath $Capture).PSIsContainer) { throw "capture must be a directory: $Capture" }
if (-not $Out) { $Out = Join-Path $Capture "replay" }
else { $Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out) }
$bin = Join-Path $repo "build\afw-replay"
New-Item -ItemType Directory -Force -Path $bin | Out-Null
. (Join-Path $PSScriptRoot "lib\msvc.ps1")
$msvc = Enter-BvrMsvcEnv
Push-Location $bin
try {
    $exe = Join-Path $bin "afw-replay.exe"
    $src = Join-Path $PSScriptRoot "afw-replay.cpp"
    $warp = Join-Path $repo "src\game\bioshock1r\afw_gpu.cpp"
    $needsBuild = -not (Test-Path $exe) -or (Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime -or
             (Get-Item $warp).LastWriteTime -gt (Get-Item $exe).LastWriteTime
    if ($needsBuild) {
        & $msvc.Cl /nologo /EHsc /W4 /O2 /std:c++17 /permissive- /I (Join-Path $repo "src") "/Fe:$exe" $src $warp `
            /link d3d11.lib d3dcompiler.lib | Where-Object { $_ -notmatch '^[\w\-]+\.cpp$' }
        if ($LASTEXITCODE -ne 0) { throw "afw-replay compilation failed" }
    }
    & $exe $Capture $Out $Mask $Gain $Stale $Grid ([int][bool]$Tint) ([int](-not $NoHeld)) ([int](-not $NoFresh)) ([int](-not $NoPictures)) $FgTan $Refine
    if ($LASTEXITCODE -ne 0) { throw "afw-replay failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
    Exit-BvrMsvcEnv $msvc
}


if ($Delete) {
    # Verify the final absolute target and every reparse boundary immediately before removal.
    $target = Get-Item -LiteralPath $Capture -Force
    $resolved = [IO.Path]::GetFullPath($target.FullName).TrimEnd([char]'\')
    if ($resolved -ne $Capture.TrimEnd([char]'\') -or
        $resolved -eq [IO.Path]::GetPathRoot($resolved).TrimEnd([char]'\') -or
        -not (Test-Path -LiteralPath (Join-Path $resolved "p00.txt") -PathType Leaf)) {
        throw "refusing to delete a root or a changed/non-capture target: $Capture"
    }
    for ($node = $target; $null -ne $node; $node = $node.Parent) {
        if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "refusing to delete through a reparse point: $($node.FullName)"
        }
    }
    $links = @(Get-ChildItem -LiteralPath $resolved -Force -Recurse |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    if ($links.Count) { throw "refusing capture deletion: a reparse point is inside $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    Write-Output "Deleted successfully replayed capture: $resolved"
}
