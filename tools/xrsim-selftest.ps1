# xrsim-selftest.ps1 - is the SIMULATOR healthy?
#
# Run this before debugging anything else. It exercises the runtime with
# xr_hello32, which is a client with no mod in it at all, so a failure here
# means the sim is broken and the mod is not the suspect.
#
# Exit codes mirror xr_hello32's own contract:
#   0  the simulator works
#   1  the SIMULATOR is broken - do not go looking in the mod
#
# Note it uses a SCRATCH directory by default, so a self-test can never clobber
# the state.json of a live game session.
#
# Usage:
#   .\tools\xrsim-selftest.ps1
[CmdletBinding()]
param(
    [switch]$Release,
    [string]$Dir = "$env:LOCALAPPDATA\BioshockVR\xrsim\selftest",
    [switch]$KeepFiles
)

$ErrorActionPreference = 'Stop'
$repo   = Split-Path -Parent $PSScriptRoot
$config = if ($Release) { "RelWithDebInfo" } else { "Debug" }
$hello  = Join-Path $repo "build\src\$config\xr_hello32.exe"

function Fail($msg) {
    Write-Host ""
    Write-Host "SELFTEST FAIL: $msg" -ForegroundColor Red
    Write-Host "The SIMULATED RUNTIME is broken. Do not blame the mod." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $hello)) { Fail "xr_hello32.exe not found at $hello - run .\tools\build.ps1 first." }

$install = & (Join-Path $PSScriptRoot "xrsim-install.ps1") -Release:$Release -Dir $Dir
Write-Host "testing $($install.Dll)"

$saved  = $env:XR_RUNTIME_JSON
$savedD = $env:BVR_XRSIM_DIR
$out = $null
$code = 1
try {
    $env:XR_RUNTIME_JSON = $install.Manifest
    $env:BVR_XRSIM_DIR   = $Dir
    $out = & $hello 2>&1 | Out-String
    $code = $LASTEXITCODE
} finally {
    $env:XR_RUNTIME_JSON = $saved
    $env:BVR_XRSIM_DIR   = $savedD
}

Write-Host $out

# Catches "the loader picked the real runtime anyway" before anything else.
if ($out -notmatch "runtime:\s*bvr-xrsim") {
    Fail "xr_hello32 did not report runtime 'bvr-xrsim'. XR_RUNTIME_JSON did not take (elevated shell? bad manifest path? 64-bit dll?)."
}

# Unlike a real runtime, the sim must ALWAYS report a system and always reach
# READY, so xr_hello32's exit 2 ("no headset connected") is a sim bug here, not
# a missing headset.
if ($code -eq 2) { Fail "xr_hello32 exited 2 (partial). The sim must always present a system and reach READY." }
if ($code -ne 0) { Fail "xr_hello32 exited $code." }
if ($out -notmatch "FULL PASS") { Fail "xr_hello32 did not report FULL PASS." }
if ($out -notmatch "Meta Quest 3") { Fail "the sim did not report a Meta Quest 3 system." }

$statePath = Join-Path $Dir "state.json"
if (-not (Test-Path $statePath)) { Fail "no state.json was written to $Dir." }
try { $s = Get-Content $statePath -Raw | ConvertFrom-Json }
catch { Fail "state.json is not valid JSON: $_" }

# xr_hello32 runs exactly 60 frames. Allow a little slack: state.json is written
# on a rate limiter in free-run mode, so the count on disk can trail the true
# final frame by a few even with the teardown flush.
if ([int]$s.frame -lt 55) { Fail "state.json reports only $($s.frame) frames; expected ~60." }
if ([int]$s.errors -ne 0) { Fail "the sim recorded $($s.errors) error(s): $($s.lastCmdError)" }

if (-not $KeepFiles) { Remove-Item (Join-Path $Dir "command.txt") -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "SELFTEST PASS: bvr-xrsim ran $($s.frame) frames, reached $($s.sessionState), 0 errors." -ForegroundColor Green
exit 0
