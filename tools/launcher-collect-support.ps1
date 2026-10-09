param(
    [Parameter(Mandatory=$true)][string]$GameDir,
    [Parameter(Mandatory=$true)][string]$DataDir,
    [Parameter(Mandatory=$true)][string]$GameIni,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
# .NET avoids dependence on the caller's PSModulePath (PowerShell 7 can pass
# an incompatible module search path to Windows PowerShell 5.1).
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Get-SupportHash([string]$Path) {
    $stream=[IO.File]::OpenRead($Path)
    $hasher=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
if (-not $OutDir) { $OutDir = Join-Path ([Environment]::GetFolderPath('Desktop')) 'BioShockVR Support' }
$outFull = [IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $outFull | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stage = Join-Path $outFull ('support-' + $stamp + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$copied = @()
function Copy-SupportFile([string]$From, [string]$Name) {
    if (-not (Test-Path -LiteralPath $From -PathType Leaf)) { return }
    $item = Get-Item -LiteralPath $From
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.Length -gt 32MB) { return }
    Copy-Item -LiteralPath $From -Destination (Join-Path $stage $Name)
    $script:copied += $Name
}
foreach ($name in @('bioshockvr.log','bioshockvr.prev.log','bioshockvr-launcher.log')) {
    Copy-SupportFile (Join-Path $DataDir $name) $name
}
if (Test-Path -LiteralPath $DataDir -PathType Container) {
    foreach ($ini in Get-ChildItem -LiteralPath $DataDir -Filter '*.ini' -File) {
        Copy-SupportFile $ini.FullName $ini.Name
    }
}
Copy-SupportFile (Join-Path $GameDir 'bioshockvr-install.json') 'installation.json'
Copy-SupportFile $GameIni 'game-Bioshock.ini'
$details = [ordered]@{ Product='BioShock Remastered VR'; CollectedUtc=[DateTime]::UtcNow.ToString('o'); GameDir=$GameDir; DataDir=$DataDir; Files=@() }
foreach ($name in @('xinput1_3.dll','bioshockvr.dll','bioshockvr.dll.disabled','bvr_steamvr32.dll','openvr_api.dll')) {
    $file = Join-Path $GameDir $name
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        $details.Files += [ordered]@{ Name=$name; Size=(Get-Item -LiteralPath $file).Length; SHA256=(Get-SupportHash $file) }
    }
}
$details.Copied = $copied
[IO.File]::WriteAllText((Join-Path $stage 'support.json'), ($details | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $stage 'READ-ME.txt'), "Review this archive before sharing it. It contains mod logs, settings, game configuration and file hashes. No save files, game assets or crash dumps are included. Nothing was uploaded.`r`n", [Text.UTF8Encoding]::new($false))
$zip = $stage + '.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($stage,$zip)
# The freshly created staging folder is retained beside the ZIP for inspection.
Write-Output ('Support ZIP: ' + $zip)
