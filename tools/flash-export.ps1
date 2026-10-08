# flash-export.ps1 - decompile a game's Scaleform UI movies with JPEXS FFDec, into the
# LOCAL model workspace. Offline only: never launches a game, never changes its files.
#
#   .\tools\flash-export.ps1 -Game bs1 -ListMovies                 # every UI movie the game ships
#   .\tools\flash-export.ps1 -Game bs1 -Movie HUDPC                # XML structure + ActionScript + frames
#   .\tools\flash-export.ps1 -Game bs1 -Movie HUDPC -What xml      # just the tag tree (fast)
#
# What it is for: the HUD, menus and radial wheels are Flash movies, so their layout
# (stage size, the clips and where they sit), their ActionScript (which engine callbacks
# they expect) and their artwork answer HUD-anchoring questions without a launch. The
# Dishonored mod measured its HUD anchors this way.
#
# BS1 ships its movies as LOOSE .swf files in ContentBaked\pc\FlashMovies (each with a
# .gsc beside it), so FFDec reads them directly - no package extraction step. BS2 and
# Infinite were not checked on a machine with them installed: -ListMovies says what it
# finds, and an Infinite movie inside a cooked package would need tools\model-export.ps1
# with UModel's -3rdparty first.
#
# Output: <model_workspace>\flash\<game>\<Movie>\ (xml\, scripts\, frames\). Everything
# there is game-derived and never enters the repo. Static frames do not run engine
# callbacks, so a frame shows the authored layout, not what the game populates at run time.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$Movie = "",
    [ValidateSet("all", "xml", "script", "frame")][string]$What = "all",
    [switch]$ListMovies,
    [int]$TimeoutSec = 600
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
$ffdec   = Get-BvrTool ffdec
$content = Get-BvrTool "content_$Game"
$ws      = Get-BvrTool model_workspace

# Filtered by extension afterwards: -Include is unreliable with -LiteralPath (it returned
# every package here), and a wildcard -Path is unsafe on a path that holds brackets.
$movies = @(Get-ChildItem -LiteralPath $content -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in @('.swf', '.gfx') })
if ($ListMovies -or -not $Movie) {
    Write-Output "$($movies.Count) UI movie(s) under $content :"
    $movies | Sort-Object Name | ForEach-Object { "  {0,-40} {1,10:N0} bytes" -f $_.Name, $_.Length } | Write-Output
    if (-not $Movie) { exit 0 }
}
$src = $movies | Where-Object { $_.BaseName -ieq $Movie } | Select-Object -First 1
if (-not $src) { throw "No movie named '$Movie' under $content. Run with -ListMovies." }

$out = Join-Path $ws "flash\$Game\$($src.BaseName)"
New-Item -ItemType Directory -Force -Path $out | Out-Null
# A .jar path means "run through java"; the CLI exe takes the same arguments.
function Invoke-Ffdec([string[]]$FArgs) {
    $exe = $ffdec; $all = $FArgs
    if ($ffdec -like '*.jar') { $exe = Get-BvrTool java; $all = @('-jar', $ffdec) + $FArgs }
    $quoted = $all | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    # FFDec prints a line per exported item; it goes to a log beside the output.
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -PassThru `
                       -RedirectStandardOutput (Join-Path $out 'ffdec.log') `
                       -RedirectStandardError (Join-Path $out 'ffdec.err.log')
    # PowerShell 5.1 reports a null ExitCode unless the handle was read before the exit.
    $null = $p.Handle
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); throw "ffdec did not finish in $TimeoutSec s" }
    if ($p.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $out 'ffdec.err.log') -Tail 10 -ErrorAction SilentlyContinue | Write-Output
        throw "ffdec exited $($p.ExitCode): $($FArgs -join ' ')"
    }
}

if ($What -in @('all', 'xml')) {
    $xml = Join-Path $out "$($src.BaseName).xml"
    Invoke-Ffdec @('-swf2xml', $src.FullName, $xml)
    Write-Output "xml     -> $xml"
}
if ($What -in @('all', 'script')) {
    Invoke-Ffdec @('-export', 'script', (Join-Path $out 'scripts'), $src.FullName)
    Write-Output "scripts -> $(Join-Path $out 'scripts')"
}
if ($What -in @('all', 'frame')) {
    Invoke-Ffdec @('-export', 'frame', (Join-Path $out 'frames'), $src.FullName)
    Write-Output "frames  -> $(Join-Path $out 'frames')"
}
$n = @(Get-ChildItem -LiteralPath $out -Recurse -File).Count
if ($n -eq 0) { throw "ffdec exited 0 but wrote nothing under $out" }
Write-Output "$n file(s) under $out"
