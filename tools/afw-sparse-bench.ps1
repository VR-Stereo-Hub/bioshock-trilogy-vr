# Synthetic masked-rendering experiment. Does not start any game or runtime.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
foreach ($name in 'BioshockHD','Bioshock2HD','BioShockInfinite') {
    if (Get-Process -Name $name -ErrorAction SilentlyContinue) { throw "$name is running; GPU timings would be invalid" }
}
$out = Join-Path $repo 'build/afw-sparse-bench'
New-Item -ItemType Directory -Force -Path $out | Out-Null
. (Join-Path $PSScriptRoot 'lib/msvc.ps1')
$msvc = Enter-BvrMsvcEnv
Push-Location $out
try {
    $exe = Join-Path $out 'afw-sparse-bench.exe'
    & $msvc.Cl /nologo /EHsc /W4 /O2 /std:c++17 /permissive- /I (Join-Path $repo 'src') (Join-Path $repo 'tools/afw-sparse-bench.cpp') "/Fe:$exe" /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Sparse experiment compile failed' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Sparse experiment failed' }
} finally {
    Pop-Location
    Exit-BvrMsvcEnv $msvc
}
