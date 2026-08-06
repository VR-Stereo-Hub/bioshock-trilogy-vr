# Layer-1 pad sweep: drive one Touch control at a time through the simulated
# runtime and read back the exact composed XInput word from `vrinput padlog`.
#
# This is the pad map's correctness claim in its most direct form: no game
# effect, no screenshot, no save state - press the control, read the bit.
#
# Usage: padsweep.ps1 -Repo <worktree> [-Game bs1|bs2|bsi] [-Only <name>]
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
    [string]$Only = ""
)

$ErrorActionPreference = "Stop"
$logPath = if ($Game -eq "bs1") { "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log" }
           else { "$env:LOCALAPPDATA\BioshockVR\$Game\bioshockvr.log" }
$xrsim = Join-Path $Repo "tools\xrsim-cmd.ps1"

function Get-LogLen { (Get-Content $logPath -ErrorAction SilentlyContinue).Count }

# One row = a label, the sim lines that drive it, and how long to let it settle.
# The sim's own hold defaults (150 ms btn, 200 ms trigger) are too short against
# a log line, so every press names an explicit hold.
$rows = @(
    @{ n = "faceA";        c = @("btn a press 500") },
    @{ n = "faceB";        c = @("btn b press 500") },
    @{ n = "faceX";        c = @("btn x press 500") },
    @{ n = "faceY";        c = @("btn y press 500") },
    @{ n = "clickL";       c = @("click l down") ; after = @("click l up") },
    @{ n = "clickR";       c = @("click r down") ; after = @("click r up") },
    @{ n = "gripL-0.60";   c = @("grip l 0.60") },
    @{ n = "gripL-0.75";   c = @("grip l 0.75") },
    @{ n = "gripL-0.60b";  c = @("grip l 0.60") },
    @{ n = "gripL-0.50";   c = @("grip l 0.50") },
    @{ n = "gripR-1.0";    c = @("grip r 1.0") ;   after = @("grip r 0") },
    @{ n = "trigL-1.0";    c = @("trigger l 1.0"); after = @("trigger l 0") },
    @{ n = "trigR-1.0";    c = @("trigger r 1.0"); after = @("trigger r 0") },
    @{ n = "stickL-fwd";   c = @("stick l 0 1") ;  after = @("stick l center") },
    @{ n = "stickL-back";  c = @("stick l 0 -1");  after = @("stick l center") },
    @{ n = "stickR-right"; c = @("stick r 1 0") ;  after = @("stick r center") },
    @{ n = "stickR-up";    c = @("stick r 0 1") ;  after = @("stick r center") },
    @{ n = "menu-tap";     c = @("btn menu press 250") },
    @{ n = "menu-hold";    c = @("btn menu down"); wait = 1.2; after = @("btn menu up") },
    # The modifier + flick lane. Thumbrest held for the whole block; each flick
    # returns to centre so the re-arm band is crossed between directions.
    @{ n = "rest-on";      c = @("thumbrest l on") },
    @{ n = "flick-up";     c = @("stick r 0 1") ;  after = @("stick r center") },
    @{ n = "flick-down";   c = @("stick r 0 -1");  after = @("stick r center") },
    @{ n = "flick-left";   c = @("stick r -1 0");  after = @("stick r center") },
    @{ n = "flick-right";  c = @("stick r 1 0") ;  after = @("stick r center") },
    @{ n = "rest-off";     c = @("thumbrest l off") }
)

& $xrsim -Quiet "input clear" | Out-Null
Start-Sleep -Milliseconds 400

foreach ($r in $rows) {
    if ($Only -and $r.n -ne $Only) { continue }
    $before = Get-LogLen
    foreach ($line in $r.c) { & $xrsim -Quiet $line | Out-Null }
    $wait = if ($r.wait) { $r.wait } else { 0.8 }
    Start-Sleep -Seconds $wait
    if ($r.after) { foreach ($line in $r.after) { & $xrsim -Quiet $line | Out-Null } }
    Start-Sleep -Milliseconds 500
    $new = Get-Content $logPath | Select-Object -Skip $before | Where-Object { $_ -match '\[input\] pad ' }
    if (-not $new) {
        "{0,-14} -> (no pad edge)" -f $r.n
    } else {
        foreach ($l in $new) {
            $m = [regex]::Match($l, '\[input\] pad (.+)$')
            "{0,-14} -> {1}" -f $r.n, $m.Groups[1].Value
        }
    }
}

& $xrsim -Quiet "input clear" | Out-Null
