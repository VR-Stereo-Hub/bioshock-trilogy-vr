# archive-symbols.ps1 - keep the exact bioshockvr.dll + .pdb pair of a build, keyed by the
# DLL's SHA-256, so a crash dump from a tester or a player can be symbolised later.
#
#   .\tools\archive-symbols.ps1                 # the RelWithDebInfo build (what package.ps1 ships)
#   .\tools\archive-symbols.ps1 -DebugBuild     # the Debug build (what build.ps1 -Install installs)
#
# A dump names the module and an offset; without the PDB of THAT exact build the offset is
# only a number. Refuses to overwrite an archive whose files differ. Local only:
# build\symbol-archive\ is gitignored with the rest of build\. Ported from the Dishonored
# VR mod.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [switch]$DebugBuild,
    [string]$OutDir = ""
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$config = if ($DebugBuild) { "Debug" } else { "RelWithDebInfo" }
$binDir = Join-Path $repo "build\src\$config"
if (-not $OutDir) { $OutDir = Join-Path $repo "build\symbol-archive" }
$dll = Join-Path $binDir 'bioshockvr.dll'
$pdb = Join-Path $binDir 'bioshockvr.pdb'
if (-not (Test-Path $dll) -or -not (Test-Path $pdb)) { throw "Matching bioshockvr.dll and .pdb are required in $binDir" }
$hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLower()
$target = Join-Path $OutDir $hash
New-Item -ItemType Directory -Path $target -Force | Out-Null
foreach ($source in @($dll, $pdb)) {
    $dest = Join-Path $target (Split-Path -Leaf $source)
    if (Test-Path -LiteralPath $dest) {
        if ((Get-FileHash -LiteralPath $dest).Hash -ne (Get-FileHash -LiteralPath $source).Hash) { throw 'Existing symbol archive differs; refusing overwrite.' }
    } else { Copy-Item -LiteralPath $source -Destination $dest }
}
@{ config = $config; dllSHA256 = $hash; pdbSHA256 = (Get-FileHash -LiteralPath $pdb).Hash; archivedUtc = [DateTime]::UtcNow.ToString('o'); fileVersion = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion } | ConvertTo-Json | Set-Content (Join-Path $target 'manifest.json')
Write-Output "Symbols archived: $target"
