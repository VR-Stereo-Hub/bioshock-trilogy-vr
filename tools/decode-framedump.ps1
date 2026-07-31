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
# THE VERTICAL SLOPE CARRIES A LETTERBOX FACTOR (session 33). The helper maps UV
# over the RENDER TARGET, so when the scene viewport is shorter than the RT the
# slope term is scaled by (RT height / viewport height) while the offset term is
# not. tanV is therefore f[o+6] (the OFFSET), and -f[o+5]/2 divided by it is the
# letterbox ratio - reported as lb=. Session 32 read that ratio as a broken
# frustum and concluded BS2's projection "degenerates" off 16:9; it does not.
# The ratio was 1.4413 = 2048/1421 in EVERY block of both square BS2 dumps,
# world and foreground alike, which is a consistent frustum plus a letterbox.
#
# Because the shipped fg lens match (vrfgfov, default ON) makes the foreground
# pass render with WORLD-equal tangents, tangents alone cannot separate the
# passes; each cluster therefore also reports how many of its draws carry the
# per-section fg-bake RVAs (0x3DBF7C / 0x3EDCBF) in their callstack, which is
# lens-independent (ENGINE_NOTES "Foreground scene FOV").
#
# THE LAYOUT IS PER-GAME (session 32). Floats 12..18 is a BS1 fact. BS2 decodes
# ZERO blocks with it, so the offset is a parameter (-RayOffset) and there are
# two instruments for deriving a new game's:
#
#   -ScanLayout   brute-force every offset in every captured block, validate the
#                 structural signature at each, and print a histogram of the
#                 offsets that pass with the tangents they yield. Finds a
#                 same-shape-different-offset layout in one run.
#   -Diff <other> compare two dumps taken at DIFFERENT FOV options and report
#                 which float indices moved, with the ratio. The projection
#                 terms are the ones whose ratio matches tan(fovA/2)/tan(fovB/2)
#                 or its reciprocal. Assumes NOTHING about layout, so this is
#                 the instrument to use when -ScanLayout comes back empty.
#
# Usage:
#   .\decode-framedump.ps1 -Path "$env:LOCALAPPDATA\BioshockVR\framedump_*_q*.txt"
#   .\decode-framedump.ps1 -Path <file> -SubmittedTanH 1.19175 -SubmittedTanV 0.67036
#   .\decode-framedump.ps1 -Path <file> -OptionFov 100 -Aspect 2048x2048
#   .\decode-framedump.ps1 -Path <bs2 dump> -ScanLayout
#   .\decode-framedump.ps1 -Path <dump @ fov 90> -Diff <dump @ fov 130> -DiffFovs 90,130
#   -ShowDraws N   lists the first N depth-tested draws per cluster (rig hunting)
#   -Tolerance     gate tolerance on |tangent - reference| (default 0.0002,
#                  the dump prints cb0 at %.4f)
param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [double]$SubmittedTanH = 0,
    [double]$SubmittedTanV = 0,
    [int]$OptionFov = 0,
    [int]$ShowDraws = 0,
    [double]$Tolerance = 0.0002,
    # Float index of the screen-ray helper block. 12 = BS1 (measured session 21).
    [int]$RayOffset = 12,
    # Backbuffer the dump was taken at, e.g. "2048x2048". The -OptionFov
    # expectation is aspect-dependent and assuming 16:9 is exactly the trap that
    # cost BS1 two sessions - given this, both candidate laws are printed.
    [string]$Aspect = "",
    # Per-game fg-bake RVAs whose presence in a callstack marks a foreground
    # draw, lens-independently. BS1's two by default; pass @() for another game.
    [string[]]$FgBakeRvas = @('3DBF7C', '3EDCBF'),
    [switch]$ScanLayout,
    [string]$Diff = "",
    [double[]]$DiffFovs = @(),
    # Session 34: per-CLUSTER cb0 dump, "lo-hi" float indices (e.g. "0-31").
    # -Diff compares whole FILES, which cannot answer a per-pass question; this
    # restricts the modal-value table to one cluster's own blocks, so two dumps
    # taken at different foreground FOVs can be compared on the FOREGROUND rows
    # alone. That is the discriminator between "the fg eye moves with the fov"
    # (transform rows change) and "the fg eye is fixed and the wider frustum
    # simply reveals more of the rig" (only the ray block changes).
    [string]$Cb0Range = ""
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$fgBakeRvas = $FgBakeRvas

$aspectW = 0.0; $aspectH = 0.0
if ($Aspect -match '^(\d+)\s*[xX]\s*(\d+)$') {
    $aspectW = [double]$Matches[1]; $aspectH = [double]$Matches[2]
}

$cb0Lo = -1; $cb0Hi = -1
if ($Cb0Range -ne '') {
    if ($Cb0Range -notmatch '^(\d+)\s*-\s*(\d+)$') { throw "-Cb0Range wants 'lo-hi', e.g. 0-31" }
    $cb0Lo = [int]$Matches[1]; $cb0Hi = [int]$Matches[2]
    if ($cb0Hi -lt $cb0Lo) { throw "-Cb0Range: hi < lo" }
    if ($cb0Hi -gt 335) { $cb0Hi = 335 }
}

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

# Parse one dump into events + captured cb0 blocks, with each event attributed
# to its governing block (a block is written whenever the VS b0 buffer OBJECT
# changes, so "most recent block at or before the draw" is exact).
function Parse-Dump($file) {
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
                    vpw  = [int]$m.Groups[7].Value; vph = [int]$m.Groups[8].Value
                    cb0b = [uint32]$m.Groups[9].Value
                    stk  = $m.Groups[14].Value
                    blk  = -1
                })
            }
        }
        if ($null -ne $curBlock) { $blocks.Add($curBlock) }
    } finally { $reader.Close() }

    $bi = 0
    for ($ei = 0; $ei -lt $events.Count; $ei++) {
        while ($bi + 1 -lt $blocks.Count -and $blocks[$bi + 1].ev -le $ei) { $bi++ }
        if ($blocks.Count -gt 0 -and $blocks[$bi].ev -le $ei) { $events[$ei].blk = $bi }
    }
    return @{ events = $events; blocks = $blocks }
}

# The screen-ray helper at floats o..o+6: (2tanH, 0, -tanH, 0, 0, -2tanV, tanV).
# The three ZERO slots (o+1, o+3, o+4) are the whole axis-disambiguation - the
# two pair checks are intra-axis and carry no information about which axis is
# which.
#
# The V pair is NOT an equality (session 33): the helper's UV runs over the
# render target, so a letterboxed viewport scales the SLOPE term and leaves the
# OFFSET term alone. tanV is the offset f[o+6]; the ratio slope/offset is the
# letterbox factor (1.000 when the viewport fills the RT). The H pair stays an
# equality - no horizontal (pillarbox) case has been observed on either game,
# so if one ever appears this check fails loudly instead of silently halving a
# tangent. Returns @(tanH, tanV, letterbox) or $null.
function Decode-RayBlock($f, [int]$o) {
    if ($f.Count -lt ($o + 7)) { return $null }
    $tanH1 = $f[$o] / 2.0; $tanH2 = -$f[$o + 2]
    $tanVs = -$f[$o + 5] / 2.0; $tanV = $f[$o + 6]
    $okH = ([math]::Abs($f[$o + 1]) -lt 0.001) -and ([math]::Abs($tanH1 - $tanH2) -lt 0.001) -and ($tanH1 -gt 0.05) -and ($tanH1 -lt 4.0)
    if (-not $okH) { return $null }
    if (([math]::Abs($f[$o + 3]) -ge 0.001) -or ([math]::Abs($f[$o + 4]) -ge 0.001)) { return $null }
    if (($tanV -le 0.05) -or ($tanV -ge 4.0) -or ($tanVs -le 0.05) -or ($tanVs -ge 8.0)) { return $null }
    $lb = $tanVs / $tanV
    # A viewport is never TALLER than its render target, so the factor is >= 1;
    # 4.0 is well past any plausible letterbox and keeps -ScanLayout specific.
    if (($lb -lt 0.999) -or ($lb -gt 4.0)) { return $null }
    return @([math]::Round($tanH1, 4), [math]::Round($tanV, 4), [math]::Round($lb, 4))
}

# Modal (most common) value at each float index across all captured blocks -
# the per-file summary the -Diff instrument compares. Values are already printed
# at %.4f in the dump, so exact string equality is the right bucketing.
function Block-Modes($blocks, [int]$n) {
    $modes = @{}
    for ($i = 0; $i -lt $n; $i++) {
        $counts = @{}
        foreach ($b in $blocks) {
            if ($b.f.Count -le $i) { continue }
            $v = $b.f[$i]
            if ([double]::IsNaN($v)) { continue }
            $k = '{0:F4}' -f $v
            if (-not $counts.ContainsKey($k)) { $counts[$k] = 0 }
            $counts[$k]++
        }
        if ($counts.Count -eq 0) { continue }
        $best = ($counts.Keys | Sort-Object { - $counts[$_] } | Select-Object -First 1)
        $modes[$i] = @{ v = [double]::Parse($best, $inv); n = $counts[$best]; distinct = $counts.Count }
    }
    return $modes
}

# ---- -Diff: derive the layout with no layout assumption at all -------------
if ($Diff) {
    $fileA = @(Get-Item -Path $Path[0])[0]
    $fileB = @(Get-Item -Path $Diff)[0]
    Write-Output ""
    Write-Output "== DIFF: $($fileA.Name)  vs  $($fileB.Name) =="
    $a = Parse-Dump $fileA
    $b = Parse-Dump $fileB
    Write-Output ("A: {0} blocks, B: {1} blocks" -f $a.blocks.Count, $b.blocks.Count)
    $ma = Block-Modes $a.blocks 336
    $mb = Block-Modes $b.blocks 336
    $expect = @()
    if ($DiffFovs.Count -eq 2) {
        $r = [math]::Tan($DiffFovs[0] * [math]::PI / 360.0) / [math]::Tan($DiffFovs[1] * [math]::PI / 360.0)
        $expect = @(@{ name = 'tan ratio'; v = $r }, @{ name = '1/tan ratio'; v = 1.0 / $r },
                    @{ name = '2x tan ratio'; v = 2.0 * $r })
        Write-Output ("FOV {0} vs {1}: tan(a/2)/tan(b/2) = {2:F6} (reciprocal {3:F6})" -f `
            $DiffFovs[0], $DiffFovs[1], $r, (1.0 / $r))
    }
    Write-Output "float indices whose modal value MOVED between the two dumps:"
    $moved = 0
    for ($i = 0; $i -lt 336; $i++) {
        if (-not $ma.ContainsKey($i) -or -not $mb.ContainsKey($i)) { continue }
        $va = $ma[$i].v; $vb = $mb[$i].v
        if ([math]::Abs($va - $vb) -le 0.0005) { continue }
        $moved++
        $ratio = if ([math]::Abs($vb) -gt 1e-9) { $va / $vb } else { [double]::NaN }
        $tag = ''
        foreach ($e in $expect) {
            if (-not [double]::IsNaN($ratio) -and [math]::Abs($ratio - $e.v) -lt 0.002) {
                $tag = "  <== $($e.name) - PROJECTION TERM"
            }
        }
        Write-Output ("  f[{0,3}]  A={1,12:F4}  B={2,12:F4}  A/B={3,10:F4}{4}" -f $i, $va, $vb, $ratio, $tag)
    }
    Write-Output "$moved indices moved. A contiguous run of flagged indices IS the ray block; its first index is -RayOffset."
    return
}

foreach ($file in $files) {
    Write-Output ""
    Write-Output "== $($file.Name) =="

    $parsed = Parse-Dump $file
    $events = $parsed.events
    $blocks = $parsed.blocks

    # ---- -ScanLayout: which offsets carry a valid ray block? ---------------
    if ($ScanLayout) {
        $hits = @{}
        foreach ($b in $blocks) {
            $maxOff = $b.f.Count - 7
            for ($o = 0; $o -lt $maxOff; $o++) {
                $t = Decode-RayBlock $b.f $o
                if ($null -eq $t) { continue }
                $k = '{0}|{1:F4}|{2:F4}|{3:F4}' -f $o, $t[0], $t[1], $t[2]
                if (-not $hits.ContainsKey($k)) { $hits[$k] = 0 }
                $hits[$k]++
            }
        }
        if ($hits.Count -eq 0) {
            Write-Output ("SCAN: no offset in any of the $($blocks.Count) captured blocks carries " +
                          "the (2tanH,0,-tanH,0,0,-2tanV,tanV) signature. This game's layout is a " +
                          "DIFFERENT SHAPE, not just a different offset - use -Diff.")
        } else {
            Write-Output "SCAN: offsets carrying a valid ray block (offset | tanH | tanV | lb | blocks):"
            foreach ($k in ($hits.Keys | Sort-Object { - $hits[$_] })) {
                $p = $k -split '\|'
                Write-Output ("  offset {0,3}  tanH={1,8}  tanV={2,8}  lb={3,7}  blocks={4}" -f `
                    $p[0], $p[1], $p[2], $p[3], $hits[$k])
            }
            Write-Output "The offset with the most blocks is the candidate for -RayOffset / the per-game constant."
        }
    }

    # ---- tangents per block ------------------------------------------------
    $blockTan = @{}
    for ($i = 0; $i -lt $blocks.Count; $i++) {
        $t = Decode-RayBlock $blocks[$i].f $RayOffset
        if ($null -ne $t) { $blockTan[$i] = $t }
    }
    if ($blockTan.Count -eq 0 -and $blocks.Count -gt 0) {
        Write-Output ("NO blocks decoded at -RayOffset $RayOffset (this default is a BioShock 1 " +
                      "fact). Re-run with -ScanLayout; if that is also empty, use -Diff against a " +
                      "dump taken at another FOV option.")
    }

    # ---- cluster depth-tested draws ---------------------------------------
    $clusters = @{}   # "tanH|tanV" -> stats
    $blockTier = @{}  # block index -> cb0 byte size (shader layout identity)
    $noBlock = 0; $drawCount = 0; $depthDraws = 0
    foreach ($ev in $events) {
        if ($drawKinds -notcontains $ev.kind) { continue }
        $drawCount++
        if ($ev.dsv -lt 0) { continue }
        $depthDraws++
        if ($ev.blk -lt 0 -or -not $blockTan.ContainsKey($ev.blk)) { $noBlock++; continue }
        # Block -> cb0 byte size, so the per-cluster cb0 table can stay inside
        # one shader layout (see the -Cb0Range print below).
        $blockTier[$ev.blk] = $ev.cb0b
        $t = $blockTan[$ev.blk]
        $key = '{0:F4}|{1:F4}' -f $t[0], $t[1]
        if (-not $clusters.ContainsKey($key)) {
            $clusters[$key] = @{
                tanH = $t[0]; tanV = $t[1]; lb = $t[2]; draws = 0; fgBake = 0
                blocks = New-Object System.Collections.Generic.HashSet[int]
                tiers = @{}; vps = @{}; sample = New-Object System.Collections.Generic.List[object]
            }
        }
        $c = $clusters[$key]
        $c.draws++
        $vpKey = '{0}x{1}' -f $ev.vpw, $ev.vph
        if (-not $c.vps.ContainsKey($vpKey)) { $c.vps[$vpKey] = 0 }
        $c.vps[$vpKey]++
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
        # hfov/vfov are 2*atan of the half-tangents, so they are the rendered
        # angles at ANY aspect - the old "@16:9" label on this line was wrong.
        $vfov = 2.0 * [math]::Atan($c.tanV) * 180.0 / [math]::PI
        Write-Output ("cluster tanH={0:F4} tanV={1:F4} (rendered {2:F1}x{3:F1} deg) lb={4:F4}  draws={5} blocks={6} fgBakeStacks={7}  b0tiers[{8}]" -f `
            $c.tanH, $c.tanV, $hfov, $vfov, $c.lb, $c.draws, $c.blocks.Count, $c.fgBake, $tierStr)
        # The letterbox check, and the ANAMORPHIC check that rides with it. The
        # frustum's aspect is tanH/tanV; the viewport's is w/h. When those two
        # disagree the render is stretched - a defect entirely separate from the
        # black band, and the one that makes a non-16:9 BS2 render unusable.
        $vpKeyBest = ($c.vps.Keys | Sort-Object { - $c.vps[$_] } | Select-Object -First 1)
        $vpParts = $vpKeyBest -split 'x'
        $vpW = [double]$vpParts[0]; $vpH = [double]$vpParts[1]
        $frustumAr = if ($c.tanV -gt 0) { $c.tanH / $c.tanV } else { [double]::NaN }
        $vpAr = if ($vpH -gt 0) { $vpW / $vpH } else { [double]::NaN }
        $lbNote = ''
        if ($aspectH -gt 0 -and $vpH -gt 0) {
            $lbExpect = $aspectH / $vpH
            $lbNote = '  expect bbH/vpH={0:F4} -> {1}' -f $lbExpect,
                      $(if ([math]::Abs($c.lb - $lbExpect) -lt 0.002) { 'OK' } else { 'MISMATCH' })
        }
        $anam = if ([math]::Abs($frustumAr - $vpAr) -lt 0.005) { 'square pixels' } else { 'ANAMORPHIC - render is stretched' }
        Write-Output ("    vp={0} (ar {1:F4})  frustum ar {2:F4}  lb={3:F4}{4}  -> {5}" -f `
            $vpKeyBest, $vpAr, $frustumAr, $c.lb, $lbNote, $anam)
        if ($ShowDraws -gt 0) {
            $c.sample | Select-Object -First $ShowDraws | ForEach-Object {
                $stkHead = ($_.stk -split ',' | Select-Object -First 4) -join ','
                Write-Output ("    #{0:D5} {1} a={2} b0={3} stk={4}" -f $_.idx, $_.kind, $_.a, $_.cb0b, $stkHead)
            }
        }
        # ---- per-cluster cb0 rows (-Cb0Range) --------------------------------
        # Modal value per float index across THIS cluster's blocks, with the
        # distinct count in brackets. A float that is constant across the pass
        # shows (1) and is a candidate camera/lens constant; one that varies per
        # draw shows a large count and is per-object.
        if ($cb0Lo -ge 0) {
            # PER TIER, and that is not a nicety. A cluster's blocks span several
            # cb0 BYTE SIZES (320/640/1280 on BS2), and a different size is a
            # different shader's constant layout - only the screen-ray helper is
            # common to all of them. Pooling them produces a modal table that
            # mixes unrelated fields and reads as noise (or worse, as a stable
            # value that is really two shaders' floats alternating).
            foreach ($tier in ($c.tiers.Keys | Sort-Object)) {
                $cBlocks = @()
                foreach ($bi in $c.blocks) {
                    if ($blockTier.ContainsKey($bi) -and $blockTier[$bi] -eq $tier) {
                        $cBlocks += $blocks[$bi]
                    }
                }
                if ($cBlocks.Count -eq 0) { continue }
                Write-Output ("    -- cb0 tier {0} B, {1} block(s) --" -f $tier, $cBlocks.Count)
                $cModes = Block-Modes $cBlocks ($cb0Hi + 1)
                $line = ''; $col = 0
                for ($i = $cb0Lo; $i -le $cb0Hi; $i++) {
                    if (-not $cModes.ContainsKey($i)) { continue }
                    $line += '{0}={1:F4}({2}) ' -f $i, $cModes[$i].v, $cModes[$i].distinct
                    $col++
                    if (($col % 6) -eq 0) { Write-Output ("       " + $line.TrimEnd()); $line = '' }
                }
                if ($line -ne '') { Write-Output ("       " + $line.TrimEnd()) }
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
            # The option-derived expectation is ASPECT-DEPENDENT and the two
            # candidate laws coincide exactly at 16:9 - which is why a single
            # aspect cannot tell them apart, and why assuming 9/16 here was
            # wrong at every aspect but one. Both laws are printed; the one that
            # PASSes at TWO different aspects is the game's.
            $tanOpt = [math]::Tan($OptionFov * [math]::PI / 360.0)
            $ar = if ($aspectW -gt 0 -and $aspectH -gt 0) { $aspectH / $aspectW } else { 9.0 / 16.0 }
            $note = if ($aspectW -gt 0) { "aspect $Aspect (h/w $('{0:F4}' -f $ar))" }
                    else { "NO -Aspect GIVEN, assuming 16:9 - pass -Aspect WxH" }
            # Law A - the option is a TRUE horizontal, vertical follows the window.
            $aH = $tanOpt; $aV = $tanOpt * $ar
            # Law B - the option is a 16:9-REFERENCED horizontal, so what the
            # engine really fixes is the VERTICAL and the horizontal follows.
            $bV = $tanOpt * 9.0 / 16.0; $bH = $bV / $ar
            foreach ($law in @(@{ n = 'A true-horizontal'; h = $aH; v = $aV },
                               @{ n = 'B 16:9-referenced'; h = $bH; v = $bV })) {
                $dh = [math]::Abs($c.tanH - $law.h); $dv = [math]::Abs($c.tanV - $law.v)
                $verdict = 'FAIL'
                if ($dh -le $Tolerance -and $dv -le $Tolerance) { $verdict = 'PASS' }
                Write-Output ("    vs OPTION {0} law {1}: tanH={2:F5} tanV={3:F5} dH={4:F5} dV={5:F5} -> {6}  [{7}]" -f `
                    $OptionFov, $law.n, $law.h, $law.v, $dh, $dv, $verdict, $note)
            }
        }
    }
}
