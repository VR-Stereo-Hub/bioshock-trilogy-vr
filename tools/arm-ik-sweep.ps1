# arm-ik-sweep.ps1 - validate the PRODUCTION arm solver (src\game\bioshock1r\arm_ik.h) on
# BS1's real arm, offline: export NEWPlayerHands and its reference skeleton, run the solver
# over a fixed pose set (tools\arm-ik-sweep.cpp), skin every frame through the original
# weights in Blender (tools\blender\arm_ik_bake.py), and render the key poses.
# The Dishonored VR mod's arm workflow; no game is launched.
#
#   .\tools\arm-ik-sweep.ps1                     # sweep + bake + key-pose renders
#   .\tools\arm-ik-sweep.ps1 -LengthScale 1.2    # the same at another arm length
#   .\tools\arm-ik-sweep.ps1 -NoRender           # numbers only
#
# Writes into <model_workspace>\ik\ and <model_workspace>\verification\ (game-derived:
# local only). Exit 1 when the solver or the mesh check fails.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [string]$Package = "0-Lighthouse",
    [string]$Mesh = "NEWPlayerHands",
    [double]$LengthScale = 1.0,
    [switch]$NoRender
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
. (Join-Path $PSScriptRoot "lib\msvc.ps1")
$py = Get-BvrTool python
$ws = Get-BvrTool model_workspace
$dir = Join-Path $ws "ik"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$glb = Join-Path $dir "$Mesh.glb"
$rig = Join-Path $dir "$Mesh.rig.txt"

if (-not (Test-Path -LiteralPath $rig) -or -not (Test-Path -LiteralPath $glb)) {
    $content = Get-BvrTool content_bs1
    $bsm = @(Get-ChildItem -LiteralPath $content -Recurse -File -Filter "$Package.bsm") | Select-Object -First 1
    if (-not $bsm) { throw "No $Package.bsm under $content" }
    $classes = Join-Path $env:LOCALAPPDATA "BioshockVR\havok\hkclasses-bs1.json"
    if (-not (Test-Path -LiteralPath $classes)) { throw "Run .\tools\bsmesh-export.ps1 once first (it reads the Havok class layouts)." }
    & $py -I (Join-Path $PSScriptRoot "bsmesh\bs2gltf.py") --bsm $bsm.FullName --classes $classes --mesh $Mesh `
        --out $glb --anims none --rig-out $rig | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "bs2gltf.py failed" }
}

$build = Join-Path $repo "build\host-tests"
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build "arm-ik-sweep.exe"
$msvc = Enter-BvrMsvcEnv
Push-Location $build
try {
    & $msvc.Cl /nologo /EHsc /W4 /O2 /std:c++17 /permissive- /I (Join-Path $repo "src") "/Fe:$exe" `
        (Join-Path $PSScriptRoot "arm-ik-sweep.cpp") | Where-Object { $_ -notmatch "^arm-ik-sweep\.cpp$" }
    if ($LASTEXITCODE -ne 0) { throw "arm-ik-sweep.cpp did not compile" }
} finally {
    Pop-Location
    Exit-BvrMsvcEnv $msvc
}

$tag = "len" + ([string]([int]($LengthScale * 100)))
$json = Join-Path $dir "sweep-$tag.json"
& $exe $rig $json $LengthScale
$sweepOk = $LASTEXITCODE -eq 0

$bake = @('--gltf', $glb, '--sweep', $json, '--out', "arm-ik-$tag.json")
if (-not $NoRender) { $bake += @('--render', "arm-ik-$tag") }
& (Join-Path $PSScriptRoot "blender-run.ps1") (Join-Path $PSScriptRoot "blender\arm_ik_bake.py") -- @bake |
    Where-Object { "$_" -like 'BVR_IKBAKE*' } | Tee-Object -Variable bakeLines | Write-Output
$bakeOk = -not ($bakeLines | Where-Object { "$_" -like 'BVR_IKBAKE problem*' }) -and ($bakeLines | Where-Object { "$_" -like 'BVR_IKBAKE *frames*' })
if (-not ($sweepOk -and $bakeOk)) { exit 1 }
