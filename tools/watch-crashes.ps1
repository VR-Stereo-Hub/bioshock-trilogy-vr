# watch-crashes.ps1 - opt-in external crash collector for a game the USER launches.
# Never launches or kills the game, and registers no global debugger.
#
#   .\tools\watch-crashes.ps1 -Game bs1                    # waits for BioshockHD.exe, arms ProcDump on it
#   .\tools\watch-crashes.ps1 -Game bs1 -Mode Memory -MemoryMB 3500   # dump when private bytes cross a line
#
# For the crashes the mod's own handler cannot record: a fault that kills the process
# before our filter runs, a hang, a memory climb toward the 4 GB LAA ceiling. Sysinternals
# ProcDump (Microsoft-signed; tool-paths: procdump) attaches to the running game, writes
# one dump on an unhandled exception (or the memory threshold) and exits; meanwhile this
# script samples private/virtual/working-set bytes and handles every 2 s to memory.csv.
# Read a dump with tools\read-dump.py. Dumps stay local, never committed.
#
# The 32-bit procdump.exe is used even if procdump64.exe is configured: the 64-bit one
# captures WOW64 host contexts that read-dump.py's x86 stack reader cannot unwind.
# Output: <data dir>\support-watch\<stamp>\ and <data dir>\dumps\.
# Ported from the Dishonored VR mod (its support-bundle step is not carried over).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$GamePath = "",
    [string]$ProcDump = "",
    [ValidateSet('Crash','Memory')][string]$Mode = 'Crash',
    [ValidateRange(128,3800)][int]$MemoryMB = 3000
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\resolve-game-path.ps1")
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
if (-not $ProcDump) { $ProcDump = Get-BvrTool procdump }
$ProcDump = (Resolve-Path -LiteralPath $ProcDump).Path
if ([IO.Path]::GetFileName($ProcDump) -ieq 'procdump64.exe') {
    $x86Dumper = Join-Path (Split-Path -Parent $ProcDump) 'procdump.exe'
    if (-not (Test-Path -LiteralPath $x86Dumper)) { throw 'Use the signed 32-bit procdump.exe from the same Microsoft package.' }
    $ProcDump = (Resolve-Path -LiteralPath $x86Dumper).Path
}
$sig = Get-AuthenticodeSignature -LiteralPath $ProcDump
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw 'ProcDump must have a valid Microsoft signature.' }

$spec = Get-BvrGameSpec -Game $Game
$gameDir = Resolve-BvrGamePath -Game $Game -GamePath $GamePath -Quiet
$gameExe = Join-Path $gameDir $spec.Exe
$procName = [IO.Path]::GetFileNameWithoutExtension($spec.Exe)
$dataDir = Join-Path $env:LOCALAPPDATA "BioshockVR"
if ($Game -ne 'bs1') { $dataDir = Join-Path $dataDir $Game }
$session = Join-Path $dataDir ('support-watch\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $session -Force | Out-Null
$dumpDir = Join-Path $dataDir 'dumps'
New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
Write-Output "Waiting for a user-launched $($spec.Exe). Mode=$Mode. Evidence: $session"
$process = $null
while (-not $process) {
    $process = Get-Process -Name $procName -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $gameExe } | Select-Object -First 1
    if (-not $process) { Start-Sleep -Seconds 1 }
}
$targetId = $process.Id
$argsList = @('-accepteula', '-n', '1', '-e', '-t')
if ($Mode -eq 'Memory') { $argsList += @('-ma', '-m', "$MemoryMB", '-s', '1') } else { $argsList += '-mm' }
$dumpPath = Join-Path $dumpDir ("external-$Mode-$targetId-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dmp')
$argsList += @("$targetId", $dumpPath)
@{ pid = $targetId; game = $Game; mode = $Mode; memoryThresholdMB = $MemoryMB; exe = $gameExe; dumperSHA256 = (Get-FileHash $ProcDump).Hash; args = $argsList; startedUtc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content (Join-Path $session 'watch.json')
$quoted = @($argsList | ForEach-Object { '"' + $_ + '"' })
$monitor = Start-Process -FilePath $ProcDump -ArgumentList $quoted -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $session 'procdump.txt') -RedirectStandardError (Join-Path $session 'procdump-errors.txt')
$null = $monitor.Handle   # PowerShell 5.1: read before exit or ExitCode comes back null
while (-not $monitor.HasExited) {
    try {
        $process.Refresh()
        if (-not $process.HasExited) {
            [pscustomobject]@{ utc = [DateTime]::UtcNow.ToString('o'); pid = $targetId; privateBytes = $process.PrivateMemorySize64; virtualBytes = $process.VirtualMemorySize64; workingSet = $process.WorkingSet64; handles = $process.HandleCount } | Export-Csv -LiteralPath (Join-Path $session 'memory.csv') -NoTypeInformation -Append
        }
    } catch { $_.Exception.Message | Add-Content (Join-Path $session 'watch-errors.txt') }
    Start-Sleep -Seconds 2
    $monitor.Refresh()
}
Write-Output "Monitor finished. Exit=$($monitor.ExitCode). Dump: $(if (Test-Path -LiteralPath $dumpPath) { $dumpPath } else { 'none written' })"
