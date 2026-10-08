# eye-seq-diff.ps1 - temporal per-eye oracle for the BS2 left-eye flicker hunt.
#
# Reads the simulator's capture\eyehash.tsv (written by the xrsim `hash every N`
# command: one row per Nth submitted XR frame, with an FNV hash of each eye's
# SOURCE projection texture, that eye's swapchain release age, and a subsampled
# mean luma) and flags the signatures the [flick]/[pair] counters are blind to:
#
#   left-stale   hashL repeated while hashR advanced  (left eye re-showed a frame)
#   right-stale  hashR repeated while hashL advanced  (the lone-left break parks
#                the RIGHT eye - the backwards-probe case)
#   eye-swap     hashL equals the previous or current hashR (tag phase offset)
#   age-spike    a projection frame submitted with a source older than MaxAge
#   luma-pop     one eye's mean luma jumped by LumaPop while the other held
#                (the HUD-burn signature: a flat menu/HUD layer landing in one
#                eye brightens that eye only)
#
# Exit code: 0 = clean, 2 = anomalies found, 1 = could not read input.
# Usage:
#   .\tools\eye-seq-diff.ps1                       # default sim capture dir
#   .\tools\eye-seq-diff.ps1 -Tsv path\eyehash.tsv -Out report.tsv
param(
    [string]$Tsv = "$env:LOCALAPPDATA\BioshockVR\xrsim\capture\eyehash.tsv",
    [string]$Out = "",
    [double]$LumaPop = 8.0,     # one-eye mean-luma jump that counts as a pop (0-255 scale)
    [double]$LumaHold = 3.0,    # the OTHER eye must move less than this to count
    [uint32]$MaxAge = 0,        # a projection frame's source older than this is a spike
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Tsv)) {
    Write-Host "eye-seq-diff: input not found: $Tsv" -ForegroundColor Red
    exit 1
}

# Share-friendly read: the sim keeps the file open (flushes per line).
$fs = [System.IO.File]::Open($Tsv, 'Open', 'Read', 'ReadWrite')
try {
    $reader = New-Object System.IO.StreamReader($fs)
    $lines = @()
    while ($null -ne ($l = $reader.ReadLine())) { $lines += $l }
} finally { $fs.Close() }

if ($lines.Count -lt 2) {
    Write-Host "eye-seq-diff: no data rows in $Tsv" -ForegroundColor Yellow
    exit 1
}

$header = $lines[0] -split "`t"
$col = @{}
for ($i = 0; $i -lt $header.Count; $i++) { $col[$header[$i]] = $i }
foreach ($need in @('frame','proj','hashL','hashR','relAgeL','relAgeR','lumaL','lumaR')) {
    if (-not $col.ContainsKey($need)) {
        Write-Host "eye-seq-diff: column '$need' missing - wrong or old TSV format" -ForegroundColor Red
        exit 1
    }
}

$anomalies = New-Object System.Collections.Generic.List[string]
$counts = @{ 'left-stale'=0; 'right-stale'=0; 'eye-swap'=0; 'age-spike'=0; 'luma-pop'=0 }
$prev = $null
$rows = 0
$projRows = 0

foreach ($line in ($lines | Select-Object -Skip 1)) {
    $f = $line -split "`t"
    if ($f.Count -lt $header.Count) { continue }
    $rows++
    $cur = [pscustomobject]@{
        Frame = [uint64]$f[$col['frame']]
        Proj  = [int]$f[$col['proj']]
        HashL = $f[$col['hashL']]
        HashR = $f[$col['hashR']]
        AgeL  = [uint32]$f[$col['relAgeL']]
        AgeR  = [uint32]$f[$col['relAgeR']]
        LumaL = [double]$f[$col['lumaL']]
        LumaR = [double]$f[$col['lumaR']]
    }

    # Only projection (stereo world) frames carry per-eye meaning; quad-mode
    # frames (menus, loading) legitimately hold one swapchain for ages.
    if ($cur.Proj -eq 1) {
        $projRows++

        if ($cur.AgeL -gt $MaxAge -or $cur.AgeR -gt $MaxAge) {
            $counts['age-spike']++
            $anomalies.Add("$($cur.Frame)`tage-spike`tageL=$($cur.AgeL) ageR=$($cur.AgeR)")
        }
        if ($cur.HashL -eq $cur.HashR -and $cur.HashL -ne '0000000000000000') {
            $counts['eye-swap']++
            $anomalies.Add("$($cur.Frame)`teye-swap`thashL==hashR (mono or copied eye) $($cur.HashL)")
        }

        if ($null -ne $prev -and $prev.Proj -eq 1) {
            $lHeld = ($cur.HashL -eq $prev.HashL)
            $rHeld = ($cur.HashR -eq $prev.HashR)
            if ($lHeld -and -not $rHeld) {
                $counts['left-stale']++
                $anomalies.Add("$($cur.Frame)`tleft-stale`thashL repeated ($($cur.HashL)) while hashR advanced")
            }
            if ($rHeld -and -not $lHeld) {
                $counts['right-stale']++
                $anomalies.Add("$($cur.Frame)`tright-stale`thashR repeated ($($cur.HashR)) while hashL advanced")
            }
            if ($cur.HashL -eq $prev.HashR -and $cur.HashL -ne '0000000000000000') {
                $counts['eye-swap']++
                $anomalies.Add("$($cur.Frame)`teye-swap`thashL equals PREVIOUS hashR ($($cur.HashL))")
            }

            $dL = [math]::Abs($cur.LumaL - $prev.LumaL)
            $dR = [math]::Abs($cur.LumaR - $prev.LumaR)
            if ($dL -ge $LumaPop -and $dR -le $LumaHold) {
                $counts['luma-pop']++
                $anomalies.Add("$($cur.Frame)`tluma-pop`tLEFT jumped $([math]::Round($dL,2)) (right held $([math]::Round($dR,2))) lumaL $([math]::Round($prev.LumaL,2))->$([math]::Round($cur.LumaL,2))")
            } elseif ($dR -ge $LumaPop -and $dL -le $LumaHold) {
                $counts['luma-pop']++
                $anomalies.Add("$($cur.Frame)`tluma-pop`tRIGHT jumped $([math]::Round($dR,2)) (left held $([math]::Round($dL,2))) lumaR $([math]::Round($prev.LumaR,2))->$([math]::Round($cur.LumaR,2))")
            }
        }
    }
    $prev = $cur
}

$total = 0
foreach ($k in $counts.Keys) { $total += $counts[$k] }

if ($Out) {
    $report = @("frame`tkind`tdetail") + $anomalies
    Set-Content -Path $Out -Value $report -Encoding utf8
}

if (-not $Quiet) {
    Write-Host ("eye-seq-diff: {0} rows ({1} projection), anomalies: {2}" -f $rows, $projRows, $total)
    foreach ($k in @('left-stale','right-stale','eye-swap','age-spike','luma-pop')) {
        $c = $counts[$k]
        if ($c -gt 0) { Write-Host ("  {0,-11} {1}" -f $k, $c) -ForegroundColor Yellow }
        else          { Write-Host ("  {0,-11} 0" -f $k) }
    }
    if ($total -gt 0) {
        Write-Host "--- first 20 anomalies ---"
        $anomalies | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
        if ($Out) { Write-Host "full report: $Out" }
    }
}

if ($total -gt 0) { exit 2 } else { exit 0 }
