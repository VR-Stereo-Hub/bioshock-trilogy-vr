# arm-ik-audit.ps1 - do BS1's arm solver and the Dishonored VR mod's put the arm in the
# same place for the same pose? Builds tools\arm-ik-audit.cpp against BOTH production
# headers (ours, and the Dishonored repo's arm_rig.h), runs them on each game's real rig
# with identical body-relative poses, and renders both meshes side by side in Blender.
#
#   .\tools\arm-ik-audit.ps1                                  # Dishonored repo at C:\dev\Dishonored-VR
#   .\tools\arm-ik-audit.ps1 -DishonoredRepo D:\src\Dishonored-VR -DishonoredPsk <arms.psk>
#   .\tools\arm-ik-audit.ps1 -UpperLength 0.71 -ForearmLength 1.13   # BS1's segments at the s86 fit
#
# Needs: the BS1 rig from .\tools\arm-ik-sweep.ps1 (run it once), the Dishonored repo
# (its assets\vr\dishonored_vr_arm_rig.bin and headers), and the Dishonored arm PSK the
# rig was prepared from (for faces). Everything written is local. Exit 1 on disagreement.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [string]$DishonoredRepo = "C:\dev\Dishonored-VR",
    [string]$DishonoredPsk = (Join-Path $env:USERPROFILE "Documents\Dishonored-VR-Arms\exports\tooltest-arms.psk"),
    [string]$Mesh = "NEWPlayerHands",
    [double]$UpperLength = 1.0,
    [double]$ForearmLength = 1.0
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
. (Join-Path $PSScriptRoot "lib\msvc.ps1")
$ws = Get-BvrTool model_workspace
$dir = Join-Path $ws "ik"
$rig = Join-Path $dir "$Mesh.rig.txt"
$glb = Join-Path $dir "$Mesh.glb"
$dhRig = Join-Path $DishonoredRepo "assets\vr\dishonored_vr_arm_rig.bin"
foreach ($p in @($rig, $glb, $dhRig, $DishonoredPsk, (Join-Path $DishonoredRepo "src\game\dishonored\hands\arm_rig.h"))) {
    if (-not (Test-Path -LiteralPath $p)) { throw "missing $p (run .\tools\arm-ik-sweep.ps1 first for the BS1 rig)" }
}

$build = Join-Path $repo "build\host-tests"
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build "arm-ik-audit.exe"
$msvc = Enter-BvrMsvcEnv
Push-Location $build
try {
    & $msvc.Cl /nologo /EHsc /W4 /O2 /std:c++17 /permissive- /I (Join-Path $repo "src") `
        /I (Join-Path $DishonoredRepo "src") "/Fe:$exe" (Join-Path $PSScriptRoot "arm-ik-audit.cpp") |
        Where-Object { $_ -notmatch "^arm-ik-audit\.cpp$" }
    if ($LASTEXITCODE -ne 0) { throw "arm-ik-audit.cpp did not compile" }
} finally {
    Pop-Location
    Exit-BvrMsvcEnv $msvc
}
$json = Join-Path $dir "audit.json"
& $exe $rig $dhRig $json $UpperLength $ForearmLength
$ok = $LASTEXITCODE -eq 0
& (Join-Path $PSScriptRoot "blender-run.ps1") (Join-Path $PSScriptRoot "blender\arm_ik_audit.py") -- `
    --audit $json --bs1-glb $glb --dh-rig $dhRig --dh-psk $DishonoredPsk |
    Where-Object { "$_" -like 'BVR_IKAUDIT*' } | Write-Output
if (-not $ok) { exit 1 }
