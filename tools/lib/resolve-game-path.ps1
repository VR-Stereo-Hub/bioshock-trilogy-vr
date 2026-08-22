# Finds a game's binary folder on THIS machine.
#
# The tools used to hardcode one contributor's drive layout (BS1 on K:, BS2 and
# Infinite on D:), so every script failed outright on any other machine with
# "Cannot find drive. A drive with the name 'K' does not exist." - and, worse,
# a stale DLL left in a folder that DID exist would keep loading while the
# install silently never happened.
#
# Resolution order, first hit wins:
#   1. -GamePath from the caller          (explicit beats everything)
#   2. $env:BVR_BS1_PATH / BS2 / BSI      (per-machine override, no edits)
#   3. Every Steam library on the box      (libraryfolders.vdf)
#   4. The usual Epic install roots        (BS1 ships as Build\FinalEpic there)
#   5. The historical hardcoded defaults   (so the original machine still works)
#
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8) and
# free of PS7-only syntax - no ternary, no ??, no ?. - it is dot-sourced by
# scripts that run under Windows PowerShell 5.1.

function Get-BvrGameSpec {
    param([ValidateSet("bs1", "bs2", "bsi")][string]$Game)

    if ($Game -eq "bs2") {
        return [pscustomobject]@{
            Exe      = "Bioshock2HD.exe"
            SteamDir = "BioShock 2 Remastered"
            SubDirs  = @("Build\Final")
            EnvVar   = "BVR_BS2_PATH"
            Legacy   = "D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final"
        }
    }
    if ($Game -eq "bsi") {
        return [pscustomobject]@{
            Exe      = "BioShockInfinite.exe"
            SteamDir = "BioShock Infinite"
            SubDirs  = @("Binaries\Win32")
            EnvVar   = "BVR_BSI_PATH"
            Legacy   = "D:\SteamLibrary\steamapps\common\BioShock Infinite\Binaries\Win32"
        }
    }
    # BS1. The Epic build lives in Build\FinalEpic, not Build\Final, so both
    # leaf folders are tried against every candidate root.
    return [pscustomobject]@{
        Exe      = "BioshockHD.exe"
        SteamDir = "BioShock Remastered"
        SubDirs  = @("Build\Final", "Build\FinalEpic")
        EnvVar   = "BVR_BS1_PATH"
        Legacy   = "K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final"
    }
}

# Every Steam library root on this machine, from Steam's own manifest.
function Get-BvrSteamLibraries {
    $roots = @()
    $steamPath = $null
    foreach ($key in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam")) {
        if (Test-Path $key) {
            $prop = Get-ItemProperty $key -ErrorAction SilentlyContinue
            if ($prop.SteamPath) { $steamPath = $prop.SteamPath }
            elseif ($prop.InstallPath) { $steamPath = $prop.InstallPath }
            if ($steamPath) { break }
        }
    }
    if (-not $steamPath) { return $roots }

    $steamPath = $steamPath -replace "/", "\"
    $roots += $steamPath
    $vdf = Join-Path $steamPath "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        # The vdf is a small nested key-value blob; the only thing needed from
        # it is every "path" value. Parsed by regex rather than by a real vdf
        # reader because that is all this needs and the format is stable.
        foreach ($line in (Get-Content $vdf -ErrorAction SilentlyContinue)) {
            if ($line -match '"path"\s+"(.+?)"') {
                $roots += ($matches[1] -replace "\\\\", "\")
            }
        }
    }
    return $roots | Select-Object -Unique
}

function Resolve-BvrGamePath {
    param(
        [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
        [string]$GamePath = "",
        [switch]$Quiet
    )

    $spec = Get-BvrGameSpec -Game $Game

    # 1. Explicit wins, and is an error if wrong - never silently search past it.
    if ($GamePath) {
        if (Test-Path (Join-Path $GamePath $spec.Exe)) { return $GamePath }
        throw "$($spec.Exe) not found in '$GamePath'."
    }

    $candidates = @()

    # 2. Per-machine override.
    $envVal = [Environment]::GetEnvironmentVariable($spec.EnvVar)
    if ($envVal) { $candidates += $envVal }

    # 3. Steam libraries.
    foreach ($root in (Get-BvrSteamLibraries)) {
        foreach ($sub in $spec.SubDirs) {
            $candidates += (Join-Path $root (Join-Path "steamapps\common" (Join-Path $spec.SteamDir $sub)))
        }
    }

    # 4. Epic. Its folder names drop the spaces that Steam keeps.
    $epicNames = @($spec.SteamDir, ($spec.SteamDir -replace " ", ""))
    foreach ($base in @("$env:ProgramFiles\Epic Games", "${env:ProgramFiles(x86)}\Epic Games", "C:\Epic Games")) {
        if (-not $base) { continue }
        foreach ($name in ($epicNames | Select-Object -Unique)) {
            foreach ($sub in $spec.SubDirs) {
                $candidates += (Join-Path $base (Join-Path $name $sub))
            }
        }
    }

    # 5. The historical default, last, so the original machine keeps working.
    $candidates += $spec.Legacy

    foreach ($c in $candidates) {
        # Test-Path throws DriveNotFound on a path whose drive is gone, which is
        # the exact failure this helper exists to absorb.
        $ok = $false
        try { $ok = Test-Path (Join-Path $c $spec.Exe) } catch { $ok = $false }
        if ($ok) {
            if (-not $Quiet) { Write-Host "Found $($spec.Exe) in $c" }
            return $c
        }
    }

    throw ("$($spec.Exe) not found. Looked in the Steam libraries, the Epic roots " +
           "and the legacy default. Pass -GamePath, or set $($spec.EnvVar).")
}
