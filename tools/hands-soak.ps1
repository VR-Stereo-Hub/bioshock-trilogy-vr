# hands-soak.ps1 - stability soak for the palette hands (s88): random controller motion, head
# turns and trigger attacks for -Seconds, then the verdict - the game alive, no faults or SEH
# catches in the log, and the palette/hand-back counters still moving. Game in gameplay on the sim.
#
#   .\tools\hands-soak.ps1 [-Seconds 120]
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([int]$Seconds = 120)
$ErrorActionPreference = 'Stop'
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"
$sim = Join-Path $PSScriptRoot "xrsim-cmd.ps1"
$mark = (Get-Content $log).Count
$rand = New-Object System.Random 4242
function Rnd($a, $b) { $a + ($b - $a) * $rand.NextDouble() }
$end = (Get-Date).AddSeconds($Seconds)
$attacks = 0; $moves = 0
while ((Get-Date) -lt $end) {
    $rp = "{0:N3} {1:N3} {2:N3} {3:N0} {4:N0} {5:N0}" -f (Rnd 0.05 0.45), (Rnd 1.1 1.7), (Rnd -0.7 -0.2), (Rnd -70 70), (Rnd -60 60), (Rnd -120 120)
    $lp = "{0:N3} {1:N3} {2:N3} {3:N0} {4:N0} {5:N0}" -f (Rnd -0.45 -0.05), (Rnd 1.1 1.7), (Rnd -0.7 -0.2), (Rnd -70 70), (Rnd -60 60), (Rnd -120 120)
    $hd = "{0:N0} {1:N0} 0" -f (Rnd -40 40), (Rnd -30 30)
    $cmds = @("hand r grip pose $rp", "hand r aim pose $rp", "hand l grip pose $lp", "hand l aim pose $lp", "head rot $hd")
    if ($rand.NextDouble() -lt 0.2) { $cmds += "trigger r 1.0"; $attacks++ }
    & $sim @cmds | Out-Null
    Start-Sleep -Milliseconds 150
    & $sim "trigger r 0" | Out-Null
    Start-Sleep -Milliseconds (Rnd 150 500)
    $moves++
    if (-not (Get-Process BioshockHD -ErrorAction SilentlyContinue)) { "FAIL: the game exited during the soak"; exit 1 }
}
$new = Get-Content $log | Select-Object -Skip $mark
$faults = @($new | Select-String "exception|FAULT|crash|access violation|SEH caught" -SimpleMatch:$false)
$composed = @($new | Select-String "\[palette\] hooked") | Select-Object -Last 1
$attacksSeen = @($new | Select-String "melee attack source=").Count
"soak: $Seconds s, $moves pose changes, $attacks trigger presses sent, $attacksSeen melee attacks classified"
"faults in the log: $($faults.Count)"
$faults | Select-Object -First 5 | ForEach-Object { "  " + $_.Line }
"last palette status: $($composed.Line)"
if ($faults.Count -eq 0 -and (Get-Process BioshockHD -ErrorAction SilentlyContinue)) { "SOAK PASS"; exit 0 } else { "SOAK FAIL"; exit 1 }
