# bsmesh-export.ps1 - a BioShock Remastered skeletal mesh, its Havok skeleton and its Havok
# animation clips -> one glTF (.glb) that Blender imports directly. Offline; never launches
# the game. What UModel cannot do for these packages (docs/MODEL_WORKFLOW.md section 2).
#
#   .\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -List          # skeletal meshes in a map
#   .\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -Mesh NEWPlayerHands
#   .\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -Mesh NEWPlayerHands -Anims "Pistol|Wrench" -Lod 0
#   .\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -Mesh ProtectorRosie -Anims none
#   .\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -Mesh NEWPlayerHands -Check   # + Blender import and renders
#
# Output: <model_workspace>\exports\<game>\<Mesh>.glb (and with -Check, a report and renders
# in verification\). The Havok class layouts the converter needs are read out of the game's
# own exe by tools\bsmesh\hkclass_dump.py on first use and cached in
# %LOCALAPPDATA%\BioshockVR\havok\hkclasses-<game>.json (game-derived: local only).
# Everything written is game-derived and stays out of the repo.
#
# Measured on BS1 2026-10-08: NEWPlayerHands (47 bones, 130 clips), ProtectorRosie (60 bones,
# 106 clips), CorpseMale (73 bones), BeaconBall_Mesh - each imported into Blender 5.2 with
# no problems and rendered posed. BS2 and Infinite are not measured: Infinite is UE3 and
# will need its own mesh reader; BS2 shares the engine and may work as-is.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1",
    [Parameter(Mandatory)][string]$Package,
    [string]$Mesh = "",
    [string]$Anims = "",
    [int]$Lod = 0,
    [double]$Scale = 1.0,
    [switch]$List,
    [switch]$Check,
    [string]$CheckAction = "",
    [switch]$RefreshClasses
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
$py = Get-BvrTool python
$content = Get-BvrTool "content_$Game"
$exe = Join-Path (Get-BvrTool "game_$Game") $(if ($Game -eq 'bs2') { 'Bioshock2HD.exe' } else { 'BioshockHD.exe' })
$bsm = @(Get-ChildItem -LiteralPath $content -Recurse -File -Filter "$Package.bsm" -ErrorAction SilentlyContinue) | Select-Object -First 1
if (-not $bsm) { throw "No $Package.bsm under $content" }

$here = Join-Path $PSScriptRoot "bsmesh"
if ($List) {
    & $py (Join-Path $here "bs2gltf.py") --bsm $bsm.FullName --classes "-" --list
    exit $LASTEXITCODE
}
if (-not $Mesh) { throw "Name -Mesh, or use -List." }

$cache = Join-Path $env:LOCALAPPDATA "BioshockVR\havok"
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$classes = Join-Path $cache "hkclasses-$Game.json"
if ($RefreshClasses -or -not (Test-Path -LiteralPath $classes)) {
    Write-Output "reading the Havok class layouts out of $exe (once per game build) ..."
    & $py (Join-Path $here "hkclass_dump.py") $exe --all --json $classes | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "hkclass_dump.py failed" }
}

$ws = Get-BvrTool model_workspace
$outDir = Join-Path $ws "exports\$Game"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir "$Mesh.glb"
$pyArgs = @((Join-Path $here "bs2gltf.py"), '--bsm', $bsm.FullName, '--classes', $classes, '--mesh', $Mesh,
            '--out', $out, '--lod', "$Lod", '--scale', "$Scale")
if ($Anims) { $pyArgs += @('--anims', $Anims) }
& $py @pyArgs
if ($LASTEXITCODE -ne 0) { throw "bs2gltf.py failed (exit $LASTEXITCODE)" }

if ($Check) {
    $chk = @('--gltf', $out, '--render', $Mesh, '--out', "$Mesh-check.json")
    if ($CheckAction) { $chk += @('--action', $CheckAction, '--frames', '0,15') }
    & (Join-Path $PSScriptRoot "blender-run.ps1") (Join-Path $PSScriptRoot "blender\gltf_check.py") -- @chk |
        Where-Object { "$_" -like 'BVR_GLTF*' } | Write-Output
}
