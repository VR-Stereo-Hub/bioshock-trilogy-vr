# decode-framedump.ps1 - recover projection tangents + pass clusters from a
# frame_inspector dump (session 21 FOV audit / world-pass instrument).
#
# Reads framedump_*_qN.txt (written by the `dumpframe full [n]` seam command),
# attributes every depth-tested draw to its governing cb0 capture (a block is
# written whenever the VS b0 buffer OBJECT changes, so "most recent block at or
# before the draw" is exact), recovers the projection tangents from the
# screen-ray helper block at cb0 floats 12..18 - layout
# (2*tanH, 0, -tanH, 0, 0, -2*tanV, tanV) - and clusters draws by tangent pair.
#
# Because the shipped fg lens match (vrfgfov, default ON) makes the foreground
# pass render with WORLD-equal tangents, tangents alone cannot separate the
# passes; each cluster therefore also reports how many of its draws carry the
# per-section fg-bake RVAs (0x3DBF7C / 0x3EDCBF) in their callstack, which is
# lens-independent (ENGINE_NOTES "Foreground scene FOV").
#
# Usage:
#   .\decode-framedump.ps1 -Path "$env:LOCALAPPDATA\BioshockVR\framedump_*_q*.txt"
#   .\decode-framedump.ps1 -Path <file> -SubmittedTanH 1.19175 -SubmittedTanV 0.67036
#   .\decode-framedump.ps1 -Path <file> -OptionFov 100
#   -ShowDraws N   lists the first N depth-tested draws per cluster (rig hunting)
#   -Tolerance     gate tolerance on |tangent - reference| (default 0.0002,
#                  the dump prints cb0 at %.4f)
param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [double]$SubmittedTanH = 0,
    [double]$SubmittedTanV = 0,
    [int]$OptionFov = 0,
    [int]$ShowDraws = 0,
    [double]$Tolerance = 0.0002
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$fgBakeRvas = @('3DBF7C', '3EDCBF')

# cb0 contents are raw buffer bytes reinterpreted as floats - garbage prints
# as nan/1.#IND and must not kill the parse. Unparseable -> NaN.
function Parse-F([string]$s) {
    $out = 0.0
    if ([double]::TryParse($s, [System.Globalization.NumberStyles]::Float, $inv, [ref]$out)) { return $out }
    return [double]::NaN
}

$files = @()
foreach ($p in $Path) { $files += @(Get-Item -Path $p | Sort-Object Name) }
if ($files.Count -eq 0) { throw "no dump files match: $Path" }

$eventRe = [regex]'^(\d{5}) (\S+)\s+a=(\d+)\s+b=(\d+)\s+rtv0=T(-?\d+)\s+dsv=T(-?\d+)\s+vp=(\d+)x(\d+) cb=(\d+)/(\d+)/(\d+) srv0=T(-?\d+)\s+ret=0x([0-9A-Fa-f]+) stk=(\S*)'
$drawKinds = @('DrawIndexed', 'Draw', 'DrawIdxInst', 'DrawInst', 'DrawIndexedInstanced', 'DrawInstanced')

foreach ($file in $files) {
    Write-Output ""
    Write-Output "== $($file.Name) =="

    # ---- parse ----------------------------------------------------------
    $events = New-Object System.Collections.Generic.List[object]
    $blocks = New-Object System.Collections.Generic.List[object]  # each: @{ev=<event idx in list>; f=double[]}
    $curBlock = $null
    $reader = New-Object System.IO.StreamReader($file.FullName)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($null -ne $curBlock) {
                if ($line -match '^\s{6,}(-?\d|nan|-nan|1\.#)' -or $line -match '^\s{6}cb0:') {
                    $nums = $line -replace '^\s*cb0:', '' -split '\s+' | Where-Object { $_ -ne '' }
                    foreach ($n in $nums) { [void]$curBlock.f.Add((Parse-F $n)) }
                    if ($curBlock.f.Count -ge 336) { $blocks.Add($curBlock); $curBlock = $null }
                    continue
                } else {
                    $blocks.Add($curBlock); $curBlock = $null  # short block (buffer < 1344 B)
                }
            }
            if ($line -match '^\s{6}cb0:') {
                $curBlock = @{ ev = $events.Count - 1; f = New-Object System.Collections.Generic.List[double] }
                $nums = $line -replace '^\s*cb0:', '' -split '\s+' | Where-Object { $_ -ne '' }
                foreach ($n in $nums) { [void]$curBlock.f.Add((Parse-F $n)) }
                continue
            }
            $m = $eventRe.Match($line)
            if ($m.Success) {
                $events.Add(@{
                    idx  = [int]$m.Groups[1].Value; kind = $m.Groups[2].Value
                    a    = [uint32]$m.Groups[3].Value; b = [uint32]$m.Groups[4].Value
                    rtv  = [int]$m.Groups[5].Value; dsv = [int]$m.Groups[6].Value
                    cb0b = [uint32]$m.Groups[9].Value
                    stk  = $m.Groups[14].Value
                    blk  = -1
                })
            }
        }
        if ($null -ne $curBlock) { $blocks.Add($curBlock) }
    } finally { $reader.Close() }

    # attribute: governing block = most recent captured block at or before the event
    $bi = 0
    for ($ei = 0; $ei -lt $events.Count; $ei++) {
        while ($bi + 1 -lt $blocks.Count -and $blocks[$bi + 1].ev -le $ei) { $bi++ }
        if ($blocks.Count -gt 0 -and $blocks[$bi].ev -le $ei) { $events[$ei].blk = $bi }
    }

    # ---- tangents per block ----------------------------------------------
    # screen-ray helper at floats 12..18: (2tanH, 0, -tanH, 0, 0, -2tanV, tanV)
    $blockTan = @{}
    for ($i = 0; $i -lt $blocks.Count; $i++) {
        $f = $blocks[$i].f
        if ($f.Count -lt 19) { continue }
        $tanH1 = $f[12] / 2.0; $tanH2 = -$f[14]
        $tanV1 = -$f[17] / 2.0; $tanV2 = $f[18]
        $okH = ([math]::Abs($f[13]) -lt 0.001) -and ([math]::Abs($tanH1 - $tanH2) -lt 0.001) -and ($tanH1 -gt 0.05) -and ($tanH1 -lt 4.0)
        $okV = ([math]::Abs($f[15]) -lt 0.001) -and ([math]::Abs($f[16]) -lt 0.001) -and ([math]::Abs($tanV1 - $tanV2) -lt 0.001) -and ($tanV1 -gt 0.05) -and ($tanV1 -lt 4.0)
        if ($okH -and $okV) { $blockTan[$i] = @([math]::Round($tanH1, 4), [math]::Round($tanV1, 4)) }
    }

    # ---- cluster depth-tested draws ---------------------------------------
    $clusters = @{}   # "tanH|tanV" -> stats
    $noBlock = 0; $drawCount = 0; $depthDraws = 0
    foreach ($ev in $events) {
        if ($drawKinds -notcontains $ev.kind) { continue }
        $drawCount++
        if ($ev.dsv -lt 0) { continue }
        $depthDraws++
        if ($ev.blk -lt 0 -or -not $blockTan.ContainsKey($ev.blk)) { $noBlock++; continue }
        $t = $blockTan[$ev.blk]
        $key = '{0:F4}|{1:F4}' -f $t[0], $t[1]
        if (-not $clusters.ContainsKey($key)) {
            $clusters[$key] = @{
                tanH = $t[0]; tanV = $t[1]; draws = 0; fgBake = 0
                blocks = New-Object System.Collections.Generic.HashSet[int]
                tiers = @{}; sample = New-Object System.Collections.Generic.List[object]
            }
        }
        $c = $clusters[$key]
        $c.draws++
        [void]$c.blocks.Add($ev.blk)
        if (-not $c.tiers.ContainsKey($ev.cb0b)) { $c.tiers[$ev.cb0b] = 0 }
        $c.tiers[$ev.cb0b]++
        $isFg = $false
        foreach ($rva in $fgBakeRvas) { if ($ev.stk -match $rva) { $isFg = $true; break } }
        if ($isFg) { $c.fgBake++ }
        if ($c.sample.Count -lt [math]::Max($ShowDraws, 6)) { $c.sample.Add($ev) }
    }

    Write-Output ("events {0}, draws {1}, depth-tested {2}, captured cb0 blocks {3}" -f `
        $events.Count, $drawCount, $depthDraws, $blocks.Count)
    if ($noBlock -gt 0) { Write-Output "depth-tested draws with no decodable screen-ray block: $noBlock" }

    foreach ($key in ($clusters.Keys | Sort-Object { - $clusters[$_].draws })) {
        $c = $clusters[$key]
        $tierStr = ($c.tiers.Keys | Sort-Object | ForEach-Object { '{0}:{1}' -f $_, $c.tiers[$_] }) -join ' '
        $hfov = 2.0 * [math]::Atan($c.tanH) * 180.0 / [math]::PI
        Write-Output ("cluster tanH={0:F4} tanV={1:F4} (hfov {2:F1} deg @16:9)  draws={3} blocks={4} fgBakeStacks={5}  b0tiers[{6}]" -f `
            $c.tanH, $c.tanV, $hfov, $c.draws, $c.blocks.Count, $c.fgBake, $tierStr)
        if ($ShowDraws -gt 0) {
            $c.sample | Select-Object -First $ShowDraws | ForEach-Object {
                $stkHead = ($_.stk -split ',' | Select-Object -First 4) -join ','
                Write-Output ("    #{0:D5} {1} a={2} b0={3} stk={4}" -f $_.idx, $_.kind, $_.a, $_.cb0b, $stkHead)
            }
        }
        # ---- gates ---------------------------------------------------------
        if ($SubmittedTanH -gt 0) {
            $dh = [math]::Abs($c.tanH - $SubmittedTanH); $dv = [math]::Abs($c.tanV - $SubmittedTanV)
            $verdict = 'FAIL'
            if ($dh -le $Tolerance -and $dv -le $Tolerance) { $verdict = 'PASS' }
            Write-Output ("    vs SUBMITTED tanH={0:F5} tanV={1:F5}: dH={2:F5} dV={3:F5} -> {4}" -f `
                $SubmittedTanH, $SubmittedTanV, $dh, $dv, $verdict)
        }
        if ($OptionFov -gt 0) {
            $oH = [math]::Tan($OptionFov * [math]::PI / 360.0); $oV = $oH * 9.0 / 16.0
            $dh = [math]::Abs($c.tanH - $oH); $dv = [math]::Abs($c.tanV - $oV)
            $verdict = 'FAIL'
            if ($dh -le $Tolerance -and $dv -le $Tolerance) { $verdict = 'PASS' }
            Write-Output ("    vs OPTION {0} tanH={1:F5} tanV={2:F5}: dH={3:F5} dV={4:F5} -> {5}" -f `
                $OptionFov, $oH, $oV, $dh, $dv, $verdict)
        }
    }
}
