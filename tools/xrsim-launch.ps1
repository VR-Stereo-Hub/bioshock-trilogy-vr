# xrsim-launch.ps1 - launch the game against the SIMULATED OpenXR runtime.
#
# A separate launcher rather than a -Sim switch on launch-game.ps1, because it
# must start the exe DIRECTLY: XR_RUNTIME_JSON is inherited from the parent
# process, and Steam launches the game itself, so going through Steam would mean
# setting the variable machine-wide - exactly what this design avoids. The
# guards are shared by CALLING launch-game.ps1 -PreflightOnly, so the two
# launchers cannot drift apart.
#
# The single most valuable check here is the runtime-name assertion. If
# XR_RUNTIME_JSON silently fails to take (an elevated shell, a bad manifest
# path, a 64-bit dll), the loader falls through to VDXR, nothing renders, and
# every later result is measured against the wrong runtime while the transcript
# claims otherwise. This throws instead.
#
# Usage:
#   $g = .\tools\xrsim-launch.ps1 -Game bs1
#   .\tools\xrsim-launch.ps1 -Game bs1 -AllowStale -WaitSeconds 120
[CmdletBinding()]
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$GamePath = "",
    [switch]$Release,
    [switch]$Force,
    [switch]$AllowStale,
    [switch]$NoInstall,
    [switch]$NoWaitSession,
    [int]$WaitSeconds = 90,
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim",
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'

$repo   = Split-Path -Parent $PSScriptRoot
$config = if ($Release) { "RelWithDebInfo" } else { "Debug" }
$proc   = switch ($Game) { "bs2" { "Bioshock2HD" } "bsi" { "BioShockInfinite" }
                           default { "BioshockHD" } }
$exeName = "$proc.exe"
$modLog = switch ($Game) { "bs2" { "$env:LOCALAPPDATA\BioshockVR\bs2\bioshockvr.log" }
                           "bsi" { "$env:LOCALAPPDATA\BioshockVR\bsi\bioshockvr.log" }
                           default { "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log" } }

if (-not $GamePath) {
    $GamePath = switch ($Game) {
        "bs2" { "D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final" }
        "bsi" { "D:\SteamLibrary\steamapps\common\BioShock Infinite\Binaries\Win32" }
        default { "K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final" }
    }
}
$exe = Join-Path $GamePath $exeName
if (-not (Test-Path $exe)) { throw "game exe not found: $exe" }

# --- guard 0: elevation ------------------------------------------------------
# The loader reads XR_RUNTIME_JSON through a SECURE env path that returns
# nothing for a high-integrity process. From an elevated shell the sim is
# silently ignored and the game runs on the real runtime.
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if ((New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "REFUSING: this shell is elevated. The OpenXR loader ignores " +
          "XR_RUNTIME_JSON in a high-integrity process, so the game would " +
          "silently run on the real runtime. Use a normal PowerShell window."
}

# --- guards 1 and 2: shared with the normal launcher -------------------------
& (Join-Path $PSScriptRoot "launch-game.ps1") -Game $Game -PreflightOnly -Force:$Force | Out-Null

# --- guard 3: the installed mod is older than what you just built -------------
$builtDll = Join-Path $repo "build\src\$config\bioshockvr.dll"
$instDll  = Join-Path $GamePath "bioshockvr.dll"
if (-not $AllowStale -and (Test-Path $builtDll) -and (Test-Path $instDll)) {
    if ((Get-Item $instDll).LastWriteTimeUtc -lt (Get-Item $builtDll).LastWriteTimeUtc) {
        throw "the INSTALLED bioshockvr.dll is older than your build - run " +
              ".\tools\install.ps1 -Game $Game first (or pass -AllowStale)."
    }
}

if (-not $NoInstall) {
    & (Join-Path $PSScriptRoot "xrsim-install.ps1") -Release:$Release -Dir $Dir | Out-Null
}
$manifest = Join-Path $Dir "bvr_xrsim32.json"
if (-not (Test-Path $manifest)) { throw "no manifest at $manifest - run xrsim-install.ps1." }

# --- launch ------------------------------------------------------------------
$logBefore = if (Test-Path $modLog) { (Get-Item $modLog).LastWriteTimeUtc } else { [datetime]::MinValue }

$savedRuntime = $env:XR_RUNTIME_JSON
$savedDir     = $env:BVR_XRSIM_DIR
try {
    $env:XR_RUNTIME_JSON = $manifest
    $env:BVR_XRSIM_DIR   = $Dir
    Write-Host "launching $exeName directly with XR_RUNTIME_JSON -> the simulator"
    # -ArgumentList rejects an empty collection, so only pass it when there is
    # something to pass.
    $p = if ($ExtraArgs -and $ExtraArgs.Count -gt 0) {
        Start-Process -FilePath $exe -WorkingDirectory $GamePath -ArgumentList $ExtraArgs -PassThru
    } else {
        Start-Process -FilePath $exe -WorkingDirectory $GamePath -PassThru
    }
} finally {
    # Restore immediately, so nothing else this shell runs inherits the sim.
    $env:XR_RUNTIME_JSON = $savedRuntime
    $env:BVR_XRSIM_DIR   = $savedDir
}

Start-Sleep -Seconds 5
if ($p.HasExited) {
    throw "the game exited within 5 s of a direct launch (exit $($p.ExitCode)). " +
          "Is the Steam client running? Start Steam, but do NOT launch the game from it."
}

if ($NoWaitSession) {
    return [pscustomobject]@{ Pid = $p.Id; Game = $Game; Exe = $exe; Log = $modLog; Dir = $Dir }
}

# --- wait for the mod to report a live session -------------------------------
# The revert-Options 'Message' dialog blocks the game before it ever presents, so
# a launcher that only waits will time out with a misleading "no session came up".
# Dismiss it here, the same way boot.ps1 does, rather than making every caller
# remember to. The dialog is a Remastered (BS1/BS2) prompt only - Infinite has no
# such dialog, so the probe is skipped there (its real launch hazard is the
# UNATTENDED attract-mode hang, which no dialog probe can fix - keep a driver at
# the menu).
Add-Type -ErrorAction SilentlyContinue @'
using System; using System.Runtime.InteropServices;
public static class BvrXrSimWin {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

$runtimeName = $null
$sessionUp   = $false
for ($i = 0; $i -lt $WaitSeconds; $i++) {
    $dlg = if ($Game -ne "bsi") { [BvrXrSimWin]::FindWindow("#32770", "Message") }
           else { [IntPtr]::Zero }
    if ($dlg -ne [IntPtr]::Zero) {
        $no = [BvrXrSimWin]::FindWindowEx($dlg, [IntPtr]::Zero, "Button", "&No")
        if ($no -ne [IntPtr]::Zero) {
            [BvrXrSimWin]::SendMessage($no, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Write-Host "dismissed the revert-Options dialog with No"
        }
    }
    if (Test-Path $modLog) {
        $lines = Get-Content $modLog -ErrorAction SilentlyContinue
        foreach ($l in $lines) {
            if ($l -match "xr: instance created on runtime '([^']+)'") { $runtimeName = $Matches[1] }
            if ($l -match "xr: session running") { $sessionUp = $true }
        }
        if ($runtimeName -and $runtimeName -ne "bvr-xrsim") {
            throw "the simulator was NOT picked up - the mod is on runtime " +
                  "'$runtimeName'. XR_RUNTIME_JSON did not take: elevated shell, " +
                  "a bad manifest path, or a 64-bit dll. Every measurement from " +
                  "here would be against the wrong runtime."
        }
        if ($sessionUp) { break }
    }
    Start-Sleep -Seconds 1
}

if (-not $runtimeName) { throw "the mod never logged an XR instance within $WaitSeconds s (log: $modLog)." }
if (-not $sessionUp) {
    $hint = if ($Game -eq "bsi") {
        "Infinite presents from the menu, so no dialog should be blocking - check the log " +
        "for bring-up refusals, and make sure the menu is ATTENDED (the attract-mode hang " +
        "stops presents entirely)."
    } else {
        "The game may be sitting on the revert-Options dialog - dismiss it, or run " +
        "boot.ps1 -Attach."
    }
    throw "runtime '$runtimeName' loaded but no session came up within $WaitSeconds s. $hint"
}

# --- confirm frames are actually advancing -----------------------------------
$statePath = Join-Path $Dir "state.json"
function Read-State {
    for ($k = 0; $k -lt 5; $k++) {
        try { return Get-Content $statePath -Raw -ErrorAction Stop | ConvertFrom-Json } catch { Start-Sleep -Milliseconds 120 }
    }
    throw "could not read $statePath"
}
$s1 = Read-State
Start-Sleep -Seconds 1
$s2 = Read-State
if ($s2.frame -le $s1.frame) {
    throw "the simulator is loaded but produced no frames in 1 s (frame stuck at $($s1.frame))."
}

Write-Host "runtime '$runtimeName', session $($s2.sessionState), frame $($s2.frame) (+$($s2.frame - $s1.frame)/s)"

[pscustomobject]@{
    Pid          = $p.Id
    Game         = $Game
    Exe          = $exe
    Log          = $modLog
    Dir          = $Dir
    Runtime      = $runtimeName
    SessionState = $s2.sessionState
    Frame        = $s2.frame
}
