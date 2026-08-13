# Build a release zip from the CURRENT tree, reproducibly.
# Releases used to be hand-assembled in a session scratchpad, which is how the
# version string drifted three releases behind the tag (session 23). This script
# reads the version from CMakeLists.txt so the zip name, the DLL banner and the
# tag cannot disagree.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [string]$OutDir = "$PSScriptRoot\..\dist",
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

# Version is the single source of truth in CMakeLists.txt.
$cml = Get-Content "$repo\CMakeLists.txt" -Raw
if ($cml -notmatch 'project\(BioshockVR\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "could not read VERSION from CMakeLists.txt"
}
$version = $Matches[1]
"version: $version"

if (-not $SkipBuild) { & "$repo\tools\build.ps1" -Release }

$bin = "$repo\build\src\RelWithDebInfo"
foreach ($n in @("bioshockvr.dll", "xinput1_3.dll")) {
    if (-not (Test-Path "$bin\$n")) { throw "missing build output: $bin\$n" }
}

# Refuse to ship a DLL whose compiled-in version does not match CMakeLists.txt.
$gen = Get-Content "$repo\build\generated\bvr_version.h" -Raw
if ($gen -notmatch '#define\s+BVR_VERSION\s+"([^"]+)"') { throw "cannot read generated version" }
if ($Matches[1] -ne $version) {
    throw "generated header says $($Matches[1]) but CMakeLists.txt says $version - rebuild"
}

$stage = "$OutDir\bioshock-vr-v$version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
# v0.7.0: one zip, two games - a preset folder per game (the BS2 one is
# optional-by-design, its values are also baked into the DLL).
New-Item -ItemType Directory -Path "$stage\preset-bs1" -Force | Out-Null
New-Item -ItemType Directory -Path "$stage\preset-bs2" -Force | Out-Null
New-Item -ItemType Directory -Path "$stage\preset-bsi" -Force | Out-Null

Copy-Item "$bin\bioshockvr.dll" $stage
Copy-Item "$bin\xinput1_3.dll"  $stage
Copy-Item "$repo\README.md" "$stage\README.txt"
Copy-Item "$repo\docs\TROUBLESHOOTING.md" "$stage\TROUBLESHOOTING.txt"
foreach ($n in @("vrpreset.ini", "hands.ini", "weapons.ini", "HOW-TO-USE.txt")) {
    Copy-Item "$repo\release\preset-bs1\$n" "$stage\preset-bs1\$n"
}
foreach ($n in @("vrpreset.ini", "weapons.ini", "HOW-TO-USE.txt")) {
    Copy-Item "$repo\release\preset-bs2\$n" "$stage\preset-bs2\$n"
}
# v0.8.0: BioShock Infinite joins the zip (early access) - preset optional-by-
# design like BS2, defaults are baked into the DLL.
foreach ($n in @("vrpreset.ini", "HOW-TO-USE.txt")) {
    Copy-Item "$repo\release\preset-bsi\$n" "$stage\preset-bsi\$n"
}

$zip = "$OutDir\bioshock-vr-v$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force

"packaged: $zip"
"{0} bytes" -f (Get-Item $zip).Length
"sha256:  {0}" -f (Get-FileHash $zip -Algorithm SHA256).Hash
""
"contents:"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
$archive.Entries | ForEach-Object { "  {0,10}  {1}" -f $_.Length, $_.FullName }
$archive.Dispose()
