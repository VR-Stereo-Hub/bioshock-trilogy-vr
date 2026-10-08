# uscript-export.ps1 - decompile a game's UnrealScript packages into the LOCAL corpus.
#
#   .\tools\uscript-export.ps1 -Game bs1                 # every BakedScripts\pc\*.U -> tools\uscript\bs1\
#   .\tools\uscript-export.ps1 -Game bs1 -Only ShockGame # one package
#   .\tools\uscript-export.ps1 -Game bsi -Source <folder of DECOMPRESSED packages>
#   .\tools\uscript-export.ps1 -Game bs1 -Inventory      # what each package is; decompiles nothing
#
# The corpus is DECLARATIONS AND DEFAULTPROPERTIES, plus whatever function bodies UELib
# can decompile for the build: class hierarchies, property NAMES to feed a runtime
# resolver, shipped tuning values, state names. It is game-derived and never committed
# (tools\uscript\* is gitignored except its README). Findings go to ENGINE_NOTES.
#
# Sources, per game:
#   bs1  <game>\BakedScripts\pc\*.U        verified 2026-10-07: Core exports 10 classes, 0 failed
#   bs2  <game>\BakedScripts\pc\*.u        assumed from BS1's layout; not checked here
#   bsi  -Source required. Infinite's cooked packages are compressed, and UELib reads
#        them only after decompression (Dishonored needed Gildor's `decompress` tool for
#        its UE3 packages - ask before downloading it). Not checked here.
#
# ONE PROCESS PER PACKAGE: UELib can die of an uncatchable StackOverflowException on a
# package, which must kill that package alone and be recorded, not the batch.
# Needs tools\uscript-export\build.ps1 to have built ExportScripts.exe once.
# Never launches a game. Ported from the Dishonored VR mod's local export.ps1.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$Source = "",
    [string]$Only = "",
    [switch]$Inventory,
    [int]$TimeoutMin = 60
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
$exe = Join-Path (Get-BvrTool ueexplorer) "ExportScripts.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "ExportScripts.exe is not built. Run .\tools\uscript-export\build.ps1" }

if (-not $Source) {
    if ($Game -eq 'bsi') { throw "Infinite needs -Source <folder of decompressed packages> (see the header)." }
    $Source = Join-Path (Get-BvrTool "game_$Game") "BakedScripts\pc"
}
if (-not (Test-Path -LiteralPath $Source)) { throw "No package folder at $Source" }
$exts = if ($Game -eq 'bsi') { @('.xxx', '.upk', '.u') } else { @('.u') }
$pkgs = @(Get-ChildItem -LiteralPath $Source -File | Where-Object { $exts -contains $_.Extension.ToLower() } | Sort-Object Length)
if ($Only) { $pkgs = @($pkgs | Where-Object { $_.BaseName -ieq $Only }) }
if ($pkgs.Count -eq 0) { throw "No packages ($($exts -join ', ')) under $Source$(if ($Only) { " named $Only" })" }

$main   = Get-BvrMainCheckout
$outDir = Join-Path $main "tools\uscript\$Game"
$logDir = Join-Path $main "tools\uscript\_logs"
New-Item -ItemType Directory -Force -Path $outDir, $logDir | Out-Null
$log = Join-Path $logDir "export-$Game.log"
$tmp = Join-Path $logDir "_exp_tmp.txt"
"# export run $(Get-Date -Format s) game=$Game source=$Source" | Set-Content -LiteralPath $log -Encoding utf8

foreach ($p in $pkgs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $status = 'ok'
    $a = @("`"$($p.FullName)`"", "`"$outDir`"")
    if ($Inventory) { $a += '--inventory' }
    try {
        # UELib prints a stack trace to stderr for every function body it cannot decompile
        # (tens of MB for BS1) while the class itself still exports; keep it in a file.
        $proc = Start-Process -FilePath $exe -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardOutput $tmp `
                             -RedirectStandardError (Join-Path $logDir "$($p.BaseName).stderr.txt")
        $null = $proc.Handle   # PowerShell 5.1: read before exit or ExitCode comes back null
        if (-not $proc.WaitForExit($TimeoutMin * 60000)) { $proc.Kill() | Out-Null; $status = 'timeout' }
        elseif ($proc.ExitCode -ne 0) { $status = "exit$($proc.ExitCode)" }
    } catch { $status = 'crashed' }
    $sw.Stop()
    $res = $null
    if (Test-Path -LiteralPath $tmp) {
        $res = (Get-Content -LiteralPath $tmp | Where-Object { $_ -like 'RESULT *' -or $_ -like 'INV *' } | Select-Object -Last 1)
    }
    if (-not $res) { $res = "RESULT $($p.Name) classes=0 failed=0 note=no-result-line" }
    $line = "{0}  [{1}, {2:N0}s]" -f $res, $status, $sw.Elapsed.TotalSeconds
    $line | Add-Content -LiteralPath $log -Encoding utf8
    Write-Output $line
}
Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
$total = @(Get-ChildItem -LiteralPath $outDir -Recurse -Filter *.uc -ErrorAction SilentlyContinue).Count
"TOTAL .uc files: $total" | Add-Content -LiteralPath $log -Encoding utf8
Write-Output "TOTAL .uc files under $outDir : $total   (log: $log)"
