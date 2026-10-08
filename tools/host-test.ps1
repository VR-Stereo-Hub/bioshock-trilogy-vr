# host-test.ps1 - compile and run pure-logic test suites on the host. No game, no
# headset, no simulator: a suite is one .cpp that includes the PRODUCTION header it
# tests (from src/) and exits non-zero on the first failed check.
#
#   .\tools\host-test.ps1                    # every tools\tests\*-tests.cpp
#   .\tools\host-test.ps1 ue-math            # one suite (tools\tests\ue-math-tests.cpp)
#
# Why it exists: the Dishonored VR mod caught frame and timing bugs in its arm IK and
# its motion sword this way that would otherwise have cost headset runs (its ARM_IK.md
# ran 1,083 assertions against the production solver). It only works for code that is
# PURE - math, state machines, detectors - so the habit it asks for is: keep the
# decision in a header with no engine reads, and test that header here.
#
# Rules a suite follows (tools\tests\ue-math-tests.cpp is the example):
#   - include the production header, never a copy of it
#   - every check prints what it compared on failure, with the numbers
#   - at least one NEGATIVE control: a check the test proves CAN fail, so a suite
#     that passes is evidence and not a tautology
# Builds into build\host-tests\ (32-bit, /W4, C++17), never into the mod.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([Parameter(Position=0)][string]$Suite = "")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $PSScriptRoot "tests"
$suites = @(Get-ChildItem -LiteralPath $dir -Filter "*-tests.cpp" | Sort-Object Name)
if ($Suite) { $suites = @($suites | Where-Object { $_.BaseName -ieq "$Suite-tests" }) }
if ($suites.Count -eq 0) { throw "no suite$(if ($Suite) { " named $Suite" }) under $dir" }

$out = Join-Path $repo "build\host-tests"
New-Item -ItemType Directory -Force -Path $out | Out-Null
. (Join-Path $PSScriptRoot "lib\msvc.ps1")
$msvc = Enter-BvrMsvcEnv
$failed = @()
Push-Location $out
try {
    foreach ($s in $suites) {
        $exe = Join-Path $out ($s.BaseName + ".exe")
        Write-Output "=== $($s.BaseName)"
        & $msvc.Cl /nologo /EHsc /W4 /std:c++17 /permissive- /I (Join-Path $repo "src") `
            /I (Join-Path $repo "third_party\OpenXR-SDK\include") "/Fe:$exe" $s.FullName |
            Where-Object { $_ -notmatch "^$([regex]::Escape($s.Name))$" }
        if ($LASTEXITCODE -ne 0) { $failed += "$($s.BaseName) (compile)"; continue }
        & $exe
        if ($LASTEXITCODE -ne 0) { $failed += $s.BaseName }
    }
} finally {
    Pop-Location
    Exit-BvrMsvcEnv $msvc
}
if ($failed.Count) { Write-Output "HOST TESTS FAILED: $($failed -join ', ')"; exit 1 }
Write-Output "host tests: $($suites.Count) suite(s) passed"
