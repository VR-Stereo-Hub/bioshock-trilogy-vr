# hand-compose-sweep.ps1 - replay real BS1 animation clips through the Dishonored hand
# model (src\game\bioshock1r\hand_compose.h) with the controller held still, solve the arms
# to the composed wrists, and skin both the game's pose and the composed one through the
# original weights in Blender. Offline; no game is launched.
#
#   .\tools\hand-compose-sweep.ps1                                 # pistol reload, shotgun, wrench, Electro
#   .\tools\hand-compose-sweep.ps1 -Clips "ReloadTommyGun|FireCrossbow"
#   .\tools\hand-compose-sweep.ps1 -NoRender
#
# Output in <model_workspace>\ik\ and verification\ (game-derived: local). Exit 1 when the
# composed wrist leaves the controller or the hand's inner pose changes.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [string]$Clips = "FastReloadPistol|ReloadShotgun_LOOP|Swing_A_Wrench$|ElectrokineticBolt_Fire$",
    [string]$Package = "0-Lighthouse",
    [string]$Mesh = "NEWPlayerHands",
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
$clipFile = Join-Path $dir "clips.txt"

$content = Get-BvrTool content_bs1
$bsm = @(Get-ChildItem -LiteralPath $content -Recurse -File -Filter "$Package.bsm") | Select-Object -First 1
if (-not $bsm) { throw "No $Package.bsm under $content" }
$classes = Join-Path $env:LOCALAPPDATA "BioshockVR\havok\hkclasses-bs1.json"
if (-not (Test-Path -LiteralPath $classes)) { throw "Run .\tools\bsmesh-export.ps1 once first (it reads the Havok class layouts)." }
& $py -I (Join-Path $PSScriptRoot "bsmesh\bs2gltf.py") --bsm $bsm.FullName --classes $classes --mesh $Mesh `
    --out $glb --anims none --rig-out $rig --clip-poses $Clips $clipFile | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "bs2gltf.py failed" }

$build = Join-Path $repo "build\host-tests"
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build "hand-compose-sweep.exe"
$msvc = Enter-BvrMsvcEnv
Push-Location $build
try {
    & $msvc.Cl /nologo /EHsc /W4 /O2 /std:c++17 /permissive- /I (Join-Path $repo "src") "/Fe:$exe" `
        (Join-Path $PSScriptRoot "hand-compose-sweep.cpp") | Where-Object { $_ -notmatch "^hand-compose-sweep\.cpp$" }
    if ($LASTEXITCODE -ne 0) { throw "hand-compose-sweep.cpp did not compile" }
} finally {
    Pop-Location
    Exit-BvrMsvcEnv $msvc
}
$native = Join-Path $dir "compose-native.json"
$composed = Join-Path $dir "compose-dishonored.json"
& $exe $rig $clipFile $native $composed
$ok = $LASTEXITCODE -eq 0
if (-not $NoRender) {
    foreach ($pair in @(@($native, "hands-game"), @($composed, "hands-dishonored"))) {
        & (Join-Path $PSScriptRoot "blender-run.ps1") (Join-Path $PSScriptRoot "blender\arm_ik_bake.py") -- `
            --gltf $glb --sweep $pair[0] --out "$($pair[1]).json" --render $pair[1] |
            Where-Object { "$_" -like 'BVR_IKBAKE*' } | Write-Output
    }
}
if (-not $ok) { exit 1 }
