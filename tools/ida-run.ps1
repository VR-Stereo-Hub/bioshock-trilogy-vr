# ida-run.ps1 - run one IDAPython script headless against a staged game binary.
#
#   .\tools\ida-run.ps1 -Game bs1 -Stage                  # copy BioshockHD.exe out of the game, analyse once, save the .i64
#   .\tools\ida-run.ps1 -Game bs1 tools\ida\pe1_callers.py   # run a script; prints where its output went
#   .\tools\ida-run.ps1 -Game bs1 -Stage -Force           # re-stage after a game update (keeps the old .i64 aside)
#
# The workspace (tool-paths: ida_workspace, default %LOCALAPPDATA%\BioshockVR\ida):
#   bin\<game>\<exe>, .i64, .md5         the staged copy and its database
#   out\<game>\<script>_out.txt, .log    output and IDA's own log
# None of it is in the repo: the binary is the game's and a decompile is game-derived.
# The SCRIPTS are ours and live in tools\ida\ (committed). docs/IDA_WORKFLOW.md is the guide.
#
# The script sees BVR_IDA_OUT (where to write) and BVR_IDA_GAME (bs1|bs2|bsi), which picks
# the known-good anchor it must reproduce before answering anything.
#
# Exit code 0 + an output file = it ran. Anything else prints the tail of IDA's log, which
# holds the Python traceback. Never launches the game, so it never contends with a running
# one and needs no conflict guard.
# Ported from the Dishonored VR mod; per game here, and RVA-based because these exes
# relocate (ASLR) where Dishonored's does not.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [Parameter(Position=0)][string]$Script = "",
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$GamePath = "",
    [switch]$Stage,
    [switch]$Force,
    [int]$TimeoutMin = 90
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
. (Join-Path $PSScriptRoot "lib\resolve-game-path.ps1")

$spec   = Get-BvrGameSpec -Game $Game
$Module = $spec.Exe
$idat = Get-BvrTool idat
$ws   = Get-BvrTool ida_workspace
$bin  = Join-Path $ws "bin\$Game"; $out = Join-Path $ws "out\$Game"
New-Item -ItemType Directory -Force -Path $bin, $out | Out-Null
$staged = Join-Path $bin $Module
$db     = "$staged.i64"
$md5f   = "$staged.md5"

function Get-Md5([string]$p) { (Get-FileHash -Algorithm MD5 -LiteralPath $p).Hash.ToLower() }

# The deployed copy, for the "is the staged copy still the game" check.
$deployed = $null
try {
    $g = Resolve-BvrGamePath -Game $Game -GamePath $GamePath -Quiet
    $cand = Join-Path $g $Module
    if (Test-Path -LiteralPath $cand) { $deployed = $cand }
} catch {}

if ($Stage) {
    if (-not $deployed) { throw "Cannot stage: $Module not found (pass -GamePath, or set $($spec.EnvVar)). Or copy it into $bin by hand." }
    $newMd5 = Get-Md5 $deployed
    if ((Test-Path -LiteralPath $staged) -and -not $Force) {
        $oldMd5 = Get-Md5 $staged
        if ($oldMd5 -ne $newMd5) { throw "Staged $Module (md5 $oldMd5) differs from the deployed one ($newMd5): a game update. Re-run with -Force; the old database is kept as .i64.$($oldMd5.Substring(0,8))." }
        Write-Output "staged $Module already matches deployed (md5 $newMd5)"
    } else {
        if (Test-Path -LiteralPath $db) {
            $keep = "$db." + (Get-Md5 $staged).Substring(0,8)
            Move-Item -LiteralPath $db -Destination $keep -Force
            Write-Output "kept the previous database as $keep"
        }
        Copy-Item -LiteralPath $deployed -Destination $staged -Force
        Write-Output "staged $deployed -> $staged (md5 $newMd5)"
    }
    Set-Content -LiteralPath $md5f -Value $newMd5 -Encoding ascii
    if (-not $Script) { $Script = Join-Path $PSScriptRoot "ida\analyze.py" }
}

if (-not $Script) { throw "Name a script (tools\ida\*.py), or pass -Stage. See docs\IDA_WORKFLOW.md." }
$scriptPath = (Resolve-Path -LiteralPath $Script).Path
$head = [IO.File]::ReadAllBytes($scriptPath)
if ($head.Length -ge 3 -and $head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF) {
    throw "$scriptPath starts with a UTF-8 BOM; IDA refuses it (SyntaxError U+FEFF). Save it without one."
}
if (-not (Test-Path -LiteralPath $staged)) { throw "$Module is not staged. Run: .\tools\ida-run.ps1 -Game $Game -Stage" }

# Provenance: is the staged copy still what the game runs?
$stagedMd5 = Get-Md5 $staged
$prov = "staged md5 $stagedMd5"
if ($deployed) {
    $depMd5 = Get-Md5 $deployed
    if ($depMd5 -eq $stagedMd5) { $prov += " == deployed (checked $(Get-Date -Format yyyy-MM-dd))" }
    else {
        $prov += " *** != deployed $depMd5 - THE GAME CHANGED; addresses from this database may be stale. Re-stage with -Stage -Force ***"
        Write-Warning $prov
    }
} else { $prov += " (deployed copy not found; not compared)" }

$name   = [IO.Path]::GetFileNameWithoutExtension($scriptPath)
$outTxt = Join-Path $out "${name}_out.txt"
$log    = Join-Path $out "$name.log"
Remove-Item -LiteralPath $outTxt, $log -ErrorAction SilentlyContinue

$target = if (Test-Path -LiteralPath $db) { "$Module.i64" } else { $Module }
if ($target -eq $Module) { Write-Output "no database yet: IDA analyses $Module first (tens of minutes for a 20 MB exe)" }
$env:BVR_IDA_OUT = $outTxt
$env:BVR_IDA_GAME = $Game
$argList = @('-A', "-S`"$scriptPath`"", "-L`"$log`"", "`"$target`"")
Write-Output "idat $($argList -join ' ')   (in $bin)"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $idat -ArgumentList $argList -WorkingDirectory $bin -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit($TimeoutMin * 60000)) {
    $p.Kill()
    throw "idat did not exit within $TimeoutMin min (a script without idc.qexit(0) never exits). Log: $log"
}
$sw.Stop()
Remove-Item Env:\BVR_IDA_OUT, Env:\BVR_IDA_GAME -ErrorAction SilentlyContinue

$code = $p.ExitCode
if ($code -eq 0 -and (Test-Path -LiteralPath $outTxt)) {
    $body = Get-Content -LiteralPath $outTxt -Raw
    $hdr  = "# ida-run $(Get-Date -Format s): $name on $Game $Module, $prov, $([int]$sw.Elapsed.TotalSeconds)s`r`n"
    [IO.File]::WriteAllText($outTxt, $hdr + $body, (New-Object Text.UTF8Encoding($false)))
    Write-Output "OK exit 0 in $([int]$sw.Elapsed.TotalSeconds)s -> $outTxt"
    exit 0
}
Write-Output "FAILED: exit $code, output file $(if (Test-Path -LiteralPath $outTxt) { 'present' } else { 'MISSING' }). Tail of $log :"
if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 30 | Write-Output }
exit $(if ($code) { $code } else { 1 })
