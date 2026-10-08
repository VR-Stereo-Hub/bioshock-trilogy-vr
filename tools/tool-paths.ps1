# tool-paths.ps1 - show, detect and set where this machine keeps the offline tools.
#
#   .\tools\tool-paths.ps1                  # table: every tool, its path, found or MISSING
#   .\tools\tool-paths.ps1 -Init            # detect what is not yet recorded and write the file
#   .\tools\tool-paths.ps1 -Init -Redetect  # also re-detect entries detection wrote before
#   .\tools\tool-paths.ps1 -Set blender=D:\Apps\Blender\blender.exe [-Set idat=...]
#   .\tools\tool-paths.ps1 -Unset blender   # forget an entry (detection may find it again)
#   .\tools\tool-paths.ps1 -Get idat        # print one path for a script; exit 1 if missing
#
# The file is LOCAL and per user (lib\tool-paths.ps1 says where and why). A session
# that finds it missing runs -Init first; a tool that is still MISSING afterwards is
# set by hand with -Set, or found online (ask before downloading anything).
# Ported from the Dishonored VR mod.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [switch]$Init,
    [switch]$Redetect,
    [string[]]$Set = @(),
    [string[]]$Unset = @(),
    [string]$Get = ""
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")

if ($Get) {
    $p = Get-BvrTool $Get -Optional
    if (-not $p) { [Console]::Error.WriteLine("Tool '$Get' is not configured or missing. Run .\tools\tool-paths.ps1 to see the table."); exit 1 }
    Write-Output $p
    exit 0
}

$cat = Get-BvrToolCatalog
$map = Read-BvrToolsFile
$dirty = $false

foreach ($n in $Unset) {
    if ($map.Contains($n)) { $map.Remove($n); $dirty = $true; Write-Output "unset $n" }
}
foreach ($kv in $Set) {
    $i = $kv.IndexOf('=')
    if ($i -lt 1) { throw "-Set takes name=path, got '$kv'" }
    $n = $kv.Substring(0, $i).Trim(); $p = $kv.Substring($i + 1).Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $p)) { throw "-Set $n : '$p' does not exist" }
    if (-not $cat.Contains($n)) { Write-Warning "'$n' is not in the catalog (lib\tool-paths.ps1); recording it anyway" }
    $map[$n] = [ordered]@{ path = (Resolve-Path -LiteralPath $p).Path; source = 'user'; set = (Get-Date -Format s) }
    $dirty = $true
    Write-Output "set $n = $($map[$n].path)"
}
if ($Init) {
    foreach ($n in $cat.Keys) {
        $have = $map.Contains($n)
        if ($have -and $map[$n].source -eq 'user') { continue }
        if ($have -and -not $Redetect -and $map[$n].path -and (Test-Path -LiteralPath $map[$n].path)) { continue }
        $p = & $cat[$n].find
        if ($p -and -not (Test-Path -LiteralPath $p) -and $cat[$n].create) { New-Item -ItemType Directory -Force -Path $p | Out-Null }
        if ($p -and (Test-Path -LiteralPath $p)) {
            $map[$n] = [ordered]@{ path = $p; source = 'detected'; set = (Get-Date -Format s) }
            $dirty = $true
        }
    }
}
if ($dirty -or ($Init -and -not (Test-Path -LiteralPath (Get-BvrToolsFile)))) {
    Write-BvrToolsFile $map
    Write-Output "wrote $(Get-BvrToolsFile)"
}

# The table. A tool the file does not record is shown with what detection WOULD find.
$file = Get-BvrToolsFile
Write-Output ""
Write-Output ("tool file: {0}{1}" -f $file, $(if (Test-Path -LiteralPath $file) { '' } else { '   (MISSING - run with -Init)' }))
$rows = foreach ($n in (@($cat.Keys) + @($map.Keys | Where-Object { -not $cat.Contains($_) }))) {
    $src = ''; $p = $null
    if ($map.Contains($n)) { $p = [string]$map[$n].path; $src = [string]$map[$n].source }
    elseif ($cat.Contains($n)) { $p = & $cat[$n].find; $src = 'not recorded' }
    $state = if ($p -and (Test-Path -LiteralPath $p)) { 'ok' } elseif ($p) { 'BAD PATH' } else { 'MISSING' }
    [pscustomobject]@{
        Group = $(if ($cat.Contains($n)) { $cat[$n].group } else { 'extra' })
        Tool = $n; State = $state; Source = $src; Path = $p
    }
}
$rows | Format-Table -AutoSize | Out-String -Width 400 | Write-Output
