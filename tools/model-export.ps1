# model-export.ps1 - pull meshes and textures out of a game's cooked packages with UModel
# (UE Viewer), into the LOCAL model workspace. Never touches the game install.
#
#   .\tools\model-export.ps1 -Game bs1 -List 0-Lighthouse -Grep "SkeletalMesh|StaticMesh"   # what a package holds
#   .\tools\model-export.ps1 -Game bs1 -Find "PlayerHands"                # which package holds it (indexes once)
#   .\tools\model-export.ps1 -Game bs1 -Package 0-Lighthouse -Object light_wall    # mesh + its textures
#   .\tools\model-export.ps1 -Game bsi -Package <pkg> -Object <mesh> -Format gltf
#
# WHAT WORKS, measured 2026-10-07 with UModel build 1590 (docs/MODEL_WORKFLOW.md):
#   bs1  textures (TGA) and static meshes (PSKX) export. SKELETAL meshes do NOT: all three
#        tried (NEWPlayerHands, BeaconBall_Mesh, CorpseMale) stop in
#        USkeletalMesh::PostLoadBioshockMesh with "Unknown Havok class: AnimationPackageRoot"
#        - the Remaster stores skinning and animation as Havok packages UModel cannot read.
#        Animations: same wall.
#   bs2  same engine branch; not measured on a machine with BS2 installed.
#   bsi  UE3; UModel lists Infinite skeletal meshes as supported, animations as not.
#        Not measured on a machine with Infinite installed.
#
# Output: <model_workspace>\originals\<game>\<Package>\<Class>\<Object>.<ext> - UModel's
# layout under a per-game folder. originals\ is the untouched extraction; edit copies in
# working\, write results to exports\. Everything here is game-derived: it stays out of the
# repo. A UModel PSK reflects Y relative to the engine's mesh space - anything compared
# with runtime data must undo it.
# Ported from the Dishonored VR mod; per game here (-game=bio / bio3 and the package
# extension differ).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$Package = "",
    [string]$Object = "",
    [ValidateSet('psk','gltf','md5')][string]$Format = 'psk',
    [switch]$NoTextures,
    [switch]$NoAnim,
    [string]$List = "",
    [string]$Grep = "",
    [string]$Find = "",
    [switch]$Reindex,
    [int]$TimeoutSec = 600
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
$umodel = Get-BvrTool umodel
$cooked = Get-BvrTool "content_$Game"
$ws     = Get-BvrTool model_workspace
$tag    = if ($Game -eq 'bsi') { 'bio3' } else { 'bio' }
# Package files UModel addresses by base name. The localised copies of every BS1/BS2 map
# (<map>_deu.bsm, ...) hold voice and text, not meshes, so -Find skips them.
$pkgGlob = if ($Game -eq 'bsi') { '*.xxx' } else { '*.bsm' }

# One UModel run, its stdout+stderr as lines. UModel prints its errors to stdout and
# returns no useful exit code on a load failure, so callers read the text.
function Invoke-UModel([string[]]$UArgs) {
    $o = [IO.Path]::GetTempFileName(); $e = [IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $umodel -ArgumentList $UArgs -NoNewWindow -PassThru `
                           -RedirectStandardOutput $o -RedirectStandardError $e
        if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); throw "umodel did not finish in $TimeoutSec s: $($UArgs -join ' ')" }
        return @(Get-Content -LiteralPath $o) + @(Get-Content -LiteralPath $e)
    } finally { Remove-Item -LiteralPath $o, $e -ErrorAction SilentlyContinue }
}
function Quote([string]$s) { return '"' + $s + '"' }

if ($List) {
    $lines = Invoke-UModel @('-list', "-path=$(Quote $cooked)", "-game=$tag", $List)
    if ($Grep) { $lines = $lines | Where-Object { $_ -match $Grep } }
    $lines | Write-Output
    exit 0
}

if ($Find) {
    # One `umodel -list` per package is slow (a BS1 map lists ~20k objects), so the
    # inventory is cached per package and reused until the package file changes.
    $idx = Join-Path $ws "index\$Game"
    New-Item -ItemType Directory -Force -Path $idx | Out-Null
    $hits = 0
    $pkgs = @(Get-ChildItem -LiteralPath $cooked -Recurse -Filter $pkgGlob |
              Where-Object { $_.BaseName -notmatch '_(chn|deu|esp|fra|int|ita|jpn|pol|rus|kor)$' })
    Write-Output "searching $($pkgs.Count) packages for /$Find/ (index: $idx)"
    foreach ($p in $pkgs) {
        $cache = Join-Path $idx ($p.BaseName + ".txt")
        if ($Reindex -or -not (Test-Path -LiteralPath $cache) -or (Get-Item -LiteralPath $cache).LastWriteTime -lt $p.LastWriteTime) {
            Invoke-UModel @('-list', "-path=$(Quote $cooked)", "-game=$tag", $p.BaseName) | Set-Content -LiteralPath $cache -Encoding utf8
        }
        foreach ($l in (Get-Content -LiteralPath $cache)) {
            if ($l -notmatch '^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)\s+(\S+)') { continue }
            # Copy the groups out FIRST: the -match against $Find below replaces $Matches
            # (the Dishonored original printed blank class and object columns for that reason).
            $cls = $Matches[1]; $obj = $Matches[2]
            if ($cls -in @('Class','ObjectProperty','NameProperty','StructProperty')) { continue }
            if ($obj -match $Find) {
                Write-Output ("{0,-34} {1,-24} {2}" -f $p.BaseName, $cls, $obj); $hits++
            }
        }
    }
    Write-Output "$hits match(es)"
    exit 0
}

if (-not $Package) { throw "Name -Package (and usually -Object), or use -List / -Find. See docs\MODEL_WORKFLOW.md." }
if (-not $Object) { Write-Warning "No -Object: exporting the WHOLE package $Package (can be large)." }
$out = Join-Path $ws "originals\$Game"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$uargs = @('-export', "-path=$(Quote $cooked)", "-out=$(Quote $out)", "-game=$tag", "-$Format")
if ($NoTextures) { $uargs += '-notex' }
if ($NoAnim) { $uargs += '-noanim' }
$uargs += $Package
if ($Object) { $uargs += $Object }
$before = Get-Date
$log = Invoke-UModel $uargs
$log | Write-Output
$err = @($log | Where-Object { $_ -match '\*\*\* ERROR' })
$new = @(Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object { $_.LastWriteTime -ge $before.AddSeconds(-2) })
Write-Output ""
if ($err.Count) {
    Write-Output "UMODEL ERROR: $($err[0])"
    if ($err[0] -match 'Havok') { Write-Output "  (a Havok-backed skeletal mesh or animation - UModel cannot read these for this game; see MODEL_WORKFLOW.md)" }
}
if ($new.Count -eq 0) {
    $why = if ($err.Count) { $err[0].Trim() } else { "no error printed - wrong -Object name or class? (check -List $Package)" }
    throw "umodel wrote nothing under $out : $why"
}
Write-Output "wrote $($new.Count) file(s):"
$new | ForEach-Object { "  {0}  ({1:N0} bytes)" -f $_.FullName, $_.Length } | Write-Output
if ($err.Count) { exit 1 }
