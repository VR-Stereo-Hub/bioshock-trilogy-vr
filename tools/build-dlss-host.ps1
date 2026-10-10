# Build the 64-bit DLSS helper (src\tools\dlss_host) with the x64 MSVC compiler.
# The mod itself is 32-bit only; this one executable is 64-bit because NVIDIA's NGX
# runtime is. Output: build\dlss_host\bvr_dlss_host64.exe plus a copy of the pinned
# nvngx_dlss.dll, NVIDIA's licence and the notices beside it - the folder install.ps1
# copies to <game>\bvr_dlss\. Needs tools\fetch-ngx.ps1 first.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$ngx = Join-Path $repo "third_party\ngx"
if (-not (Test-Path (Join-Path $ngx "include\nvsdk_ngx.h"))) { throw "NGX SDK missing: run tools\fetch-ngx.ps1" }
$out = Join-Path $repo "build\dlss_host"
New-Item -ItemType Directory -Force -Path $out | Out-Null
. (Join-Path $PSScriptRoot "lib\msvc.ps1")
$root = Get-BvrMsvcRoot
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdk = (Get-ChildItem "$kits\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$libv = (Get-ChildItem "$kits\Lib" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$savedInclude = $env:INCLUDE; $savedLib = $env:LIB
$env:INCLUDE = "$root\include;$sdk\ucrt;$sdk\shared;$sdk\um"
$env:LIB = "$root\lib\x64;$libv\ucrt\x64;$libv\um\x64"
Push-Location $out
try {
    # NGX's nvsdk_ngx_d.lib is built /MD; mixing it with /MT duplicates CRT state.
    & "$root\bin\Hostx64\x64\cl.exe" /nologo /O2 /EHsc /W4 /MD /std:c++17 /DUNICODE /D_UNICODE /D_WINDOWS `
        /I (Join-Path $repo "src") /I (Join-Path $ngx "include") `
        (Join-Path $repo "src\tools\dlss_host\dlss_host.cpp") /Fe:bvr_dlss_host64.exe `
        /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup (Join-Path $ngx "lib\Windows_x86_64\x64\nvsdk_ngx_d.lib") user32.lib advapi32.lib
    if ($LASTEXITCODE -ne 0) { throw "dlss host compilation failed" }
    Copy-Item (Join-Path $ngx "lib\Windows_x86_64\rel\nvngx_dlss.dll") . -Force
    Copy-Item (Join-Path $ngx "LICENSE.txt") (Join-Path $out "NVIDIA-DLSS-LICENSE.txt") -Force
    Copy-Item (Join-Path $repo "src\tools\dlss_host\NOTICE.md") . -Force
    Remove-Item -ErrorAction SilentlyContinue *.obj
    Write-Host "build-dlss-host: $out\bvr_dlss_host64.exe"
} finally {
    Pop-Location
    $env:INCLUDE = $savedInclude; $env:LIB = $savedLib
}
