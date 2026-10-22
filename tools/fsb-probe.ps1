# fsb-probe.ps1 - list a game's sound banks, or one sound's loudness over time, through the
# game's own fmodex.dll (tools\fsb-probe\fsb_probe.cpp says why).
#
#   .\tools\fsb-probe.ps1 -Filter pump                  # every subsound named *pump* in every bank
#   .\tools\fsb-probe.ps1 -Bank streams_0_audio -Name <sound> [-StepMs 10]   # its envelope
#   .\tools\fsb-probe.ps1 -File <x.unk> [-StepMs 10]   # a Sound object's own data (an MP3, exported outside the
#       repo: umodel -export -game=bio -sounds -out=<dir> 1-Welcome <sound>) - its loudness, whole
#   .\tools\fsb-probe.ps1 -Seek <x.unk> -StepMs <ms>    # how a channel seeked to <ms> maps onto the decoded audio
#
# BS1 only (the bank folder and fmodex.dll are the Remastered layout). Nothing decoded
# is written anywhere: the output is names, lengths and loudness numbers.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([string]$Filter = "", [string]$Bank = "", [string]$Name = "", [string]$File = "", [string]$Seek = "", [int]$StepMs = 10)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "lib\resolve-game-path.ps1")
$final = Resolve-BvrGamePath -Game bs1 -Quiet
$dll = Join-Path $final "fmodex.dll"
$banks = Join-Path $final "..\..\ContentBaked\pc\Sounds_Windows"

$out = Join-Path $repo "build\host-tests"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$probe = Join-Path $out "fsb_probe.exe"
$src = Join-Path $PSScriptRoot "fsb-probe\fsb_probe.cpp"
if (-not (Test-Path $probe) -or (Get-Item $src).LastWriteTime -gt (Get-Item $probe).LastWriteTime) {
    . (Join-Path $PSScriptRoot "lib\msvc.ps1")
    $msvc = Enter-BvrMsvcEnv
    Push-Location $out
    try {
        & $msvc.Cl /nologo /EHsc /W4 /std:c++17 "/Fe:$probe" $src | Write-Output
        if ($LASTEXITCODE -ne 0) { throw "fsb_probe did not compile" }
    } finally {
        Pop-Location
        Exit-BvrMsvcEnv $msvc
    }
}
if ($File) {
    # `pcm`: the whole decoded sample (readData on an open-only MP3 came back 400 ms short at the start).
    & $probe $dll pcm (Resolve-Path -LiteralPath $File).Path $StepMs
} elseif ($Seek) {
    & $probe $dll seek (Resolve-Path -LiteralPath $Seek).Path $StepMs
} elseif ($Name) {
    $f = Get-ChildItem -LiteralPath $banks -Filter "$Bank*.fsb" | Select-Object -First 1
    if (-not $f) { throw "no bank $Bank under $banks" }
    & $probe $dll env $f.FullName $Name $StepMs
} else {
    foreach ($f in Get-ChildItem -LiteralPath $banks -Filter "*.fsb") {
        $lines = & $probe $dll list $f.FullName $Filter | Where-Object { $_ -notmatch '^# nothing' }
        if ($lines) { Write-Output "== $($f.Name)"; $lines }
    }
}
