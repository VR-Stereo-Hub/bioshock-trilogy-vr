# gpu-test.ps1 - compile and run the GPU host suites: production D3D11 passes (and the DLSS
# transport) driven with synthetic scenes on THIS machine's GPU. No game, no headset, no
# simulator. The pure-logic suites stay in tools\tests\ (host-test.ps1); these need
# d3d11.lib and the production .cpp files they test, which host-test.ps1 does not link.
#
#   .\tools\gpu-test.ps1                    # every suite
#   .\tools\gpu-test.ps1 temporal-gpu       # one suite (tools\gpu-tests\temporal-gpu-tests.cpp)
#
# Each suite names its production sources in a first-line comment:
#   // gpu-test-sources: src/game/bioshock1r/temporal_gpu.cpp src/core/gfx/dlss_client.cpp
# and follows host-test.ps1's rules: the production code itself, numbers printed on
# failure, and at least one negative control per property.
# Suites that need the DLSS helper (dlss-host) build it first with build-dlss-host.ps1.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([Parameter(Position=0)][string]$Suite = "")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $PSScriptRoot "gpu-tests"
$suites = @(Get-ChildItem -LiteralPath $dir -Filter "*-tests.cpp" | Sort-Object Name)
if ($Suite) { $suites = @($suites | Where-Object { $_.BaseName -ieq "$Suite-tests" }) }
if ($suites.Count -eq 0) { throw "no suite$(if ($Suite) { " named $Suite" }) under $dir" }
foreach ($g in 'BioshockHD', 'Bioshock2HD', 'BioShockInfinite') {
    if (Get-Process $g -ErrorAction SilentlyContinue) { throw "$g is running: close it before GPU checks (they share the GPU)." }
}

$out = Join-Path $repo "build\gpu-tests"
New-Item -ItemType Directory -Force -Path $out | Out-Null
. (Join-Path $PSScriptRoot "lib\msvc.ps1")
$failed = @()
foreach ($s in $suites) {
    $text = Get-Content -LiteralPath $s.FullName -Raw
    $srcs = @(); $defs = @(); $libs = @()
    if ($text -match 'gpu-test-sources:\s*([^\r\n]*)') {
        $srcs = @($Matches[1].Trim() -split '\s+' | Where-Object { $_ } | ForEach-Object { Join-Path $repo $_ })
    }
    # Optional: preprocessor defines, and extra libraries (a path with a slash is repo-relative).
    if ($text -match 'gpu-test-defines:\s*([^\r\n]*)') {
        $defs = @($Matches[1].Trim() -split '\s+' | Where-Object { $_ } | ForEach-Object { "/D$_" })
    }
    if ($text -match 'gpu-test-libs:\s*([^\r\n]*)') {
        $libs = @($Matches[1].Trim() -split '\s+' | Where-Object { $_ } | ForEach-Object { if ($_ -match '/') { Join-Path $repo $_ } else { $_ } })
    }
    if ($text -match 'gpu-test-needs: dlss-host') {
        & (Join-Path $PSScriptRoot "build-dlss-host.ps1")
    }
    # A suite that needs the simulated OpenXR runtime gets it per process, in a scratch
    # directory (never the live game's), exactly as xrsim-selftest.ps1 does.
    $xrEnv = $null
    if ($text -match 'gpu-test-needs: xrsim') {
        $simDir = Join-Path $out "xrsim-$($s.BaseName)"
        $install = & (Join-Path $PSScriptRoot "xrsim-install.ps1") -Release -Dir $simDir
        . (Join-Path $PSScriptRoot "lib\xr-layers.ps1")
        $xrEnv = @{ Json = $env:XR_RUNTIME_JSON; Dir = $env:BVR_XRSIM_DIR; Layers = (Disable-BvrUnloadableXrLayers) }
        $env:XR_RUNTIME_JSON = $install.Manifest
        $env:BVR_XRSIM_DIR = $simDir
    }
    $msvc = Enter-BvrMsvcEnv
    Push-Location $out
    try {
        $exe = Join-Path $out ($s.BaseName + ".exe")
        Write-Output "=== $($s.BaseName)"
        & $msvc.Cl /nologo /EHsc /W4 /O2 /std:c++17 /permissive- @defs /I (Join-Path $repo "src") `
            /I (Join-Path $repo "third_party\imgui") /I (Join-Path $repo "third_party\OpenXR-SDK\include") `
            /I (Join-Path $repo "third_party\minhook\include") "/Fe:$exe" $s.FullName @srcs `
            /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib @libs |
            Where-Object { $_ -notmatch '^[\w\-]+\.cpp$' }
        if ($LASTEXITCODE -ne 0) { $failed += "$($s.BaseName) (compile)"; continue }
        & $exe $repo
        if ($LASTEXITCODE -ne 0) { $failed += $s.BaseName }
    } finally {
        Pop-Location
        Exit-BvrMsvcEnv $msvc
        if ($xrEnv) {
            $env:XR_RUNTIME_JSON = $xrEnv.Json
            $env:BVR_XRSIM_DIR = $xrEnv.Dir
            Restore-BvrXrLayerEnv $xrEnv.Layers
        }
    }
}
if ($failed.Count) { Write-Output "GPU TESTS FAILED: $($failed -join ', ')"; exit 1 }
Write-Output "gpu tests: $($suites.Count) suite(s) passed"
