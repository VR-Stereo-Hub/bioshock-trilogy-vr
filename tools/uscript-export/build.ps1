# build.ps1 - compile ExportScripts.exe against the UELib that ships inside UE Explorer.
#
#   .\tools\uscript-export\build.ps1          # writes ExportScripts.exe INTO the ueexplorer folder
#
# The exe must sit beside Eliot.UELib.dll and its dependencies, so it is built into the
# local UE Explorer folder (tool-paths: ueexplorer), never into the repo. UE Explorer is
# third-party software referenced by path: get it from
# https://github.com/UE-Explorer/UE-Explorer (1.6.2 verified) and record it with
#   .\tools\tool-paths.ps1 -Set ueexplorer=<folder holding Eliot.UELib.dll>
# Uses the .NET Framework 4 compiler that every Windows install carries; UE Explorer
# itself targets .NET Framework 4.8.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "..\lib\tool-paths.ps1")
$ue  = Get-BvrTool ueexplorer
$lib = Join-Path $ue "Eliot.UELib.dll"
if (-not (Test-Path -LiteralPath $lib)) { throw "Eliot.UELib.dll is not in $ue - point ueexplorer at the folder that holds it." }
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe"
if (-not (Test-Path -LiteralPath $csc)) { throw "No .NET Framework 4 compiler at $csc" }
$src = Join-Path $PSScriptRoot "ExportScripts.cs"
$out = Join-Path $ue "ExportScripts.exe"
& $csc /nologo /target:exe /platform:anycpu /optimize+ "/out:$out" "/reference:$lib" $src
if ($LASTEXITCODE -ne 0) { throw "csc failed (exit $LASTEXITCODE)" }
Write-Output "built $out"
