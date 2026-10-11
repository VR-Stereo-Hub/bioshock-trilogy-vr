# Fetch the NVIDIA DLSS (NGX) SDK into third_party\ngx (gitignored, never committed).
# Pinned to the release the Dishonored VR mod and the BioShock VR DLSS/DLAA fork tested:
# v310.7.0, whose lib\Windows_x86_64\rel\nvngx_dlss.dll is FileVersion 310.7.0.0, SHA256
# BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E.
# The SDK is governed by NVIDIA's license (third_party\ngx\LICENSE.txt); the runtime DLL
# may be redistributed with the mod under its terms, the headers and import libraries
# stay out of the repository.
#
#   .\tools\fetch-ngx.ps1                 # clone from GitHub (sparse, pinned)
#   .\tools\fetch-ngx.ps1 -From <dir>     # copy an existing local clone, then verify it
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([string]$From = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repo "third_party\ngx"
$tag = "v310.7.0"
$commit = "a291cc7d2cc642a51566f3dfd5376f635cd1b284"
$dllSha = "BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E"
if (-not (Test-Path (Join-Path $dest ".git"))) {
    if ($From) {
        robocopy $From $dest /E /NFL /NDL /NJH /NJS | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "copying $From failed (robocopy $LASTEXITCODE)" }
    } else {
        git clone --depth 1 --branch $tag --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git $dest
        if ($LASTEXITCODE -ne 0) { throw "git clone of the NGX SDK failed" }
        git -C $dest sparse-checkout set --no-cone /LICENSE.txt /README.md /include/ /lib/Windows_x86_64/x64/nvsdk_ngx_d.lib /lib/Windows_x86_64/rel/nvngx_dlss.dll
        if ($LASTEXITCODE -ne 0) { throw "sparse checkout of the NGX SDK failed" }
    }
}
$head = (git -C $dest rev-parse HEAD).Trim()
if ($head -ne $commit) { throw "third_party\ngx is at $head, expected $commit ($tag)" }
$dll = Join-Path $dest "lib\Windows_x86_64\rel\nvngx_dlss.dll"
$sha = (Get-FileHash $dll -Algorithm SHA256).Hash
if ($sha -ne $dllSha) { throw "nvngx_dlss.dll hash $sha does not match the pinned $dllSha" }
Write-Host "fetch-ngx: NGX SDK $tag at $commit, nvngx_dlss.dll $sha"
