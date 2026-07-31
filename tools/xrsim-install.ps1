# xrsim-install.ps1 - write the simulated runtime's manifest and prepare its
# control directory.
#
# NEVER touches the registry. The sim is selected per-process through
# XR_RUNTIME_JSON (the OpenXR loader checks that env var before the registry),
# so the machine's real ActiveRuntime stays on VDXR and putting the Quest 3 on
# still works with no switching.
#
# Usage:
#   .\tools\xrsim-install.ps1
#   .\tools\xrsim-install.ps1 -Release
#   .\tools\xrsim-install.ps1 -Dir C:\some\scratch\dir -KeepControlFiles
[CmdletBinding()]
param(
    [switch]$Release,
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [switch]$KeepControlFiles
)

$ErrorActionPreference = 'Stop'

$repo   = Split-Path -Parent $PSScriptRoot
$config = if ($Release) { "RelWithDebInfo" } else { "Debug" }
$outDir = Join-Path $repo "build\src\$config"
$dll    = Join-Path $outDir "bvr_xrsim32.dll"

if (-not (Test-Path $dll)) {
    throw "bvr_xrsim32.dll not found in $outDir - run .\tools\build.ps1$(if ($Release) {' -Release'}) first."
}

# --- bitness guard -----------------------------------------------------------
# An x64 dll here is the nastiest failure mode there is: the 32-bit loader
# silently skips it, falls through to the registry, and every later measurement
# is taken against VDXR while the transcript says "sim". Same PE read as
# tools\check-laa.ps1.
$fs = [System.IO.File]::OpenRead($dll)
try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Seek(0x3C, 'Begin') | Out-Null
    $peOffset = $br.ReadInt32()
    $fs.Seek($peOffset, 'Begin') | Out-Null
    if ($br.ReadUInt32() -ne 0x00004550) { throw "$dll is not a PE image (no PE\0\0 signature)." }
    $machine = $br.ReadUInt16()
} finally { $fs.Close() }

if ($machine -ne 0x014C) {
    throw ("$dll reports PE machine 0x{0:X4}, not 0x014C (x86). A 32-bit game " -f $machine) +
          "cannot load it, and the loader would quietly fall back to the real runtime."
}

# --- directories -------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Dir "capture") | Out-Null

# --- manifest ----------------------------------------------------------------
# ABSOLUTE library_path. A relative one resolves against the loading process's
# working directory, not the manifest's - which is why the game (launched with
# its own cwd) failed to load a relative manifest that xr_hello32 accepted.
$manifest = Join-Path $Dir "bvr_xrsim32.json"
$escaped  = $dll.Replace('\', '\\')
$json     = '{"file_format_version":"1.0.0","runtime":{"name":"bvr-xrsim","library_path":"' + $escaped + '"}}'
# No BOM: the loader's JSON parser chokes on one.
[System.IO.File]::WriteAllText($manifest, $json, [System.Text.UTF8Encoding]::new($false))

# --- stale control files -----------------------------------------------------
if (-not $KeepControlFiles) {
    foreach ($leaf in @("command.txt", "state.json", "ack.txt")) {
        $p = Join-Path $Dir $leaf
        if (Test-Path $p) {
            if ($leaf -eq "command.txt") {
                $stale = (Get-Content $p -Raw).Trim() -replace "`r?`n", " | "
                if ($stale) { Write-Host "cleared a stale command.txt (would have re-applied): $stale" }
            }
            Remove-Item $p -Force
        }
    }
}

Write-Host "manifest: $manifest"
Write-Host "dll:      $dll ($config, x86)"
Write-Host "dir:      $Dir"
Write-Host ""
Write-Host "To use it, launch the game with:  `$env:XR_RUNTIME_JSON = '$manifest'"
Write-Host "(tools\xrsim-launch.ps1 does that for you, and restores it afterwards.)"

[pscustomobject]@{
    Dir      = $Dir
    Manifest = $manifest
    Dll      = $dll
    Config   = $config
    BuiltUtc = (Get-Item $dll).LastWriteTimeUtc
}
