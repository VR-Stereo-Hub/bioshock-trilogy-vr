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
    # Two "WxH" backbuffer sizes. An INDEPENDENT diff axis from -DiffFovs, and a
    # strictly more informative one: under a true-horizontal option law tanH is
    # PINNED and tanV moves by (h1/w1)/(h2/w2); under a 16:9-referenced law the
    # reverse. One axis being pinned is itself the discriminator, and the
    # identification is the CROSS-PRODUCT of the two diffs - an index that moves
    # under FOV and is pinned under aspect is a horizontal projection term.
    [string]$DiffAspects = "",
    # Restrict the diff/scan to constant buffers of these byte sizes. On an
    # engine whose b0 is PER-OBJECT (BioShock Infinite rewrites one buffer per
    # draw) the modal value at most float indices is object noise; within a
    # single size tier the layout is fixed and a view slot really is constant.
    [int[]]$BlockBytes = @(),
    # Ignore indices whose modal value is held by less than this share of blocks.
    # A slot at 240/249 is a view constant; one at 3/249 is noise.
    [double]$MinModeShare = 0.0,
    # Recover tanH/tanV from a 4x4 transform instead of BS1's 7-float ray block.
    # UE3 almost certainly ships a matrix, so run this FIRST - it needs one dump,
    # no relaunch and no FOV change.
    [switch]$ScanMatrix,
    # Prove the scanners can find a known answer before any negative from them
    # is believed. An instrument that cannot fail its own hypothesis is not
    # evidence.
    [switch]$SelfTest,
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
$uploadRe = [regex]'^U(\d{5}) ev=(-?\d+) dst=T(-?\d+)\s+bytes=(\d+) off=(\d+) n=(\d+)'

function Parse-Dump($file) {
    $events = New-Object System.Collections.Generic.List[object]
    $blocks = New-Object System.Collections.Generic.List[object]  # each: @{ev=<event idx>; f=double[]; bytes=<int>}
    $curBlock = $null
    $mode = 'lite'
    $reader = New-Object System.IO.StreamReader($file.FullName)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -match '^frame dump:.*mode=(\w+)') { $mode = $Matches[1] }
            if ($null -ne $curBlock) {
                if ($line -match '^\s{6,}(-?\d|nan|-nan|1\.#)' -or $line -match '^\s{6}cb0:') {
                    $nums = $line -replace '^\s*cb0:', '' -split '\s+' | Where-Object { $_ -ne '' }
                    foreach ($n in $nums) { [void]$curBlock.f.Add((Parse-F $n)) }
                    # NO COUNT TERMINATOR. The old `-ge 336` cap closed the block
                    # early on any buffer larger than 1344 bytes and then SILENTLY
                    # DROPPED the remaining continuation lines, because they match
                    # neither the event regex nor a block opener - producing a
                    # plausible-looking wrong block. The else branch below already
                    # closes a block correctly on the first non-continuation line,
                    # which is always an event line or a section header.
                    continue
                } else {
                    $blocks.Add($curBlock); $curBlock = $null
                }
            }
            if ($line -match '^\s{6}cb0:') {
                $b = if ($events.Count -gt 0) { $events[$events.Count - 1].cb0b } else { 0 }
                $curBlock = @{ ev = $events.Count - 1; bytes = $b; f = New-Object System.Collections.Generic.List[double] }
                $nums = $line -replace '^\s*cb0:', '' -split '\s+' | Where-Object { $_ -ne '' }
                foreach ($n in $nums) { [void]$curBlock.f.Add((Parse-F $n)) }
                continue
            }
            # Mode 3 `== cb uploads ==` records. Same block shape, so every
            # instrument below works on them unchanged - and these are the ones
            # that matter on an engine that uploads with UpdateSubresource,
            # because they carry the real payload at its real size.
            $u = $uploadRe.Match($line)
            if ($u.Success) {
                $curBlock = @{
                    ev    = [int]$u.Groups[2].Value
                    bytes = [int]$u.Groups[4].Value
                    f     = New-Object System.Collections.Generic.List[double]
                }
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
                    # srv0 is the draw's first texture. Session 34: this is how a
                    # single mesh is told apart from its neighbours inside one
                    # pass - the foreground pass is ~17 draws carrying the weapon
                    # AND the rig, and a lens or a draw count cannot separate them.
                    srv0 = [int]$m.Groups[12].Value
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
    $maxF = 0
    $sizes = @{}
    foreach ($b in $blocks) {
        if ($b.f.Count -gt $maxF) { $maxF = $b.f.Count }
        $k = [int]$b.bytes
        if (-not $sizes.ContainsKey($k)) { $sizes[$k] = 0 }
        $sizes[$k]++
    }
    return @{ events = $events; blocks = $blocks; maxFloats = $maxF; mode = $mode; sizes = $sizes }
}

# Blocks restricted to the -BlockBytes tiers, if any were given.
function Select-Blocks($parsed) {
    if ($BlockBytes.Count -eq 0) { return $parsed.blocks }
    $keep = New-Object System.Collections.Generic.List[object]
    foreach ($b in $parsed.blocks) { if ($BlockBytes -contains [int]$b.bytes) { $keep.Add($b) } }
    return $keep
}

# One line so a truncation or tier-filter regression is VISIBLE rather than
# silent - the failure mode the old 336 cap had.
function Report-Blocks($tag, $parsed, $sel) {
    $s = ($parsed.sizes.Keys | Sort-Object | ForEach-Object { "$_ B x$($parsed.sizes[$_])" }) -join ', '
    Write-Output ("{0}: mode={1} blocks={2}{3} maxFloats={4} | tiers: {5}" -f `
        $tag, $parsed.mode, $parsed.blocks.Count,
        $(if ($BlockBytes.Count -gt 0) { " (selected $($sel.Count))" } else { "" }),
        $parsed.maxFloats, $(if ($s) { $s } else { "none" }))
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

# ---- the 4x4 route --------------------------------------------------------
#
# UE3 hands the shader a transform, not BS1's 7-float screen-ray helper. For a
# row-vector engine with M = World * View * Projection, writing c0/c1/c3 for the
# COLUMNS (M[0][k], M[1][k], M[2][k]) and s for the object scale:
#     c3 = forward * s        |c0| = s / tanH        |c1| = s / tanV
# so tanH = |c3|/|c0| and tanV = |c3|/|c1|, AND THE OBJECT SCALE CANCELS - which
# is what makes this work on a per-object constant buffer where nothing else is
# constant. Gate on the three orthogonality tests plus a sane tangent range.
# Returns @(tanH, tanV) or $null.
function Decode-Matrix($f, [int]$o, [bool]$transposed) {
    if ($f.Count -lt ($o + 16)) { return $null }
    $m = @()
    for ($r = 0; $r -lt 4; $r++) {
        $row = @()
        for ($c = 0; $c -lt 4; $c++) {
            $v = if ($transposed) { $f[$o + $c * 4 + $r] } else { $f[$o + $r * 4 + $c] }
            if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { return $null }
            $row += $v
        }
        $m += , $row
    }
    $c0 = @($m[0][0], $m[1][0], $m[2][0])
    $c1 = @($m[0][1], $m[1][1], $m[2][1])
    $c3 = @($m[0][3], $m[1][3], $m[2][3])
    $n0 = [math]::Sqrt($c0[0]*$c0[0] + $c0[1]*$c0[1] + $c0[2]*$c0[2])
    $n1 = [math]::Sqrt($c1[0]*$c1[0] + $c1[1]*$c1[1] + $c1[2]*$c1[2])
    $n3 = [math]::Sqrt($c3[0]*$c3[0] + $c3[1]*$c3[1] + $c3[2]*$c3[2])
    if ($n0 -lt 1e-6 -or $n1 -lt 1e-6 -or $n3 -lt 1e-6) { return $null }
    $d03 = ($c0[0]*$c3[0] + $c0[1]*$c3[1] + $c0[2]*$c3[2]) / ($n0 * $n3)
    $d13 = ($c1[0]*$c3[0] + $c1[1]*$c3[1] + $c1[2]*$c3[2]) / ($n1 * $n3)
    $d01 = ($c0[0]*$c1[0] + $c0[1]*$c1[1] + $c0[2]*$c1[2]) / ($n0 * $n1)
    if ([math]::Abs($d03) -gt 1e-3 -or [math]::Abs($d13) -gt 1e-3 -or [math]::Abs($d01) -gt 1e-3) { return $null }
    $tanH = $n3 / $n0
    $tanV = $n3 / $n1
    if ($tanH -le 0.05 -or $tanH -ge 4.0 -or $tanV -le 0.05 -or $tanV -ge 4.0) { return $null }
    return @([math]::Round($tanH, 4), [math]::Round($tanV, 4))
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

# ---- -SelfTest: prove the scanners can find a KNOWN answer -----------------
# Synthesise a block from known tangents and a known rotation and confirm both
# decoders recover them. If this fails, a negative from either scanner says the
# INSTRUMENT is broken, not that the game lacks the data.
if ($SelfTest) {
    Write-Output ""
    Write-Output "== self test =="
    $tH = 0.8391; $tV = 0.4720   # 80 deg horizontal at 16:9
    $s = 3.7                     # arbitrary object scale: it must cancel
    # A yawed camera basis, so the test is not accidentally axis-aligned.
    $a = 0.6
    $fwd = @([math]::Cos($a), [math]::Sin($a), 0.0)
    $rgt = @(-[math]::Sin($a), [math]::Cos($a), 0.0)
    $up = @(0.0, 0.0, 1.0)
    $f = New-Object System.Collections.Generic.List[double]
    for ($i = 0; $i -lt 5; $i++) { [void]$f.Add(0.123) }   # padding before the matrix
    for ($r = 0; $r -lt 3; $r++) {
        [void]$f.Add($rgt[$r] * $s / $tH)
        [void]$f.Add($up[$r] * $s / $tV)
        [void]$f.Add(0.0)
        [void]$f.Add($fwd[$r] * $s)
    }
    for ($c = 0; $c -lt 4; $c++) { [void]$f.Add(0.0) }     # translation row
    $hit = $null; $at = -1
    for ($o = 0; $o + 16 -le $f.Count; $o++) {
        $d = Decode-Matrix $f $o $false
        if ($null -ne $d) { $hit = $d; $at = $o; break }
    }
    if ($null -ne $hit) {
        $okH = [math]::Abs($hit[0] - $tH) -lt 0.002
        $okV = [math]::Abs($hit[1] - $tV) -lt 0.002
        Write-Output ("  ScanMatrix: {0} at offset {1} - recovered tanH={2} tanV={3} (planted {4}/{5}, scale {6} cancelled)" -f `
            $(if ($okH -and $okV) { "PASS" } else { "FAIL" }), $at, $hit[0], $hit[1], $tH, $tV, $s)
    } else {
        Write-Output "  ScanMatrix: FAIL - did not find the planted matrix at any offset"
    }
    # The ray-block decoder, same treatment.
    $g = New-Object System.Collections.Generic.List[double]
    for ($i = 0; $i -lt 7; $i++) { [void]$g.Add(0.777) }
    [void]$g.Add(2 * $tH); [void]$g.Add(0.0); [void]$g.Add(-$tH); [void]$g.Add(0.0)
    [void]$g.Add(0.0); [void]$g.Add(-2 * $tV); [void]$g.Add($tV)
    $hit2 = $null; $at2 = -1
    for ($o = 0; $o + 7 -le $g.Count; $o++) {
        $d = Decode-RayBlock $g $o
        if ($null -ne $d) { $hit2 = $d; $at2 = $o; break }
    }
    if ($null -ne $hit2) {
        Write-Output ("  ScanLayout: PASS at offset {0} - recovered tanH={1} tanV={2}" -f $at2, $hit2[0], $hit2[1])
    } else {
        Write-Output "  ScanLayout: FAIL - did not find the planted ray block"
    }
    Write-Output ""
    if ($Path.Count -eq 0) { return }
}

# ---- -ScanMatrix: brute-force a 4x4 across every block ---------------------
if ($ScanMatrix) {
    foreach ($file in $files) {
        $parsed = Parse-Dump $file
        $sel = Select-Blocks $parsed
        Write-Output ""
        Report-Blocks $file.Name $parsed $sel
        $hits = @{}
        foreach ($b in $sel) {
            $maxOff = $b.f.Count - 16
            for ($o = 0; $o -le $maxOff; $o++) {
                foreach ($tr in @($false, $true)) {
                    $t = Decode-Matrix $b.f $o $tr
                    if ($null -eq $t) { continue }
                    $k = '{0}|{1}|{2:F4}|{3:F4}' -f $o, $(if ($tr) { 'T' } else { 'R' }), $t[0], $t[1]
                    if (-not $hits.ContainsKey($k)) { $hits[$k] = 0 }
                    $hits[$k]++
                }
            }
        }
        if ($hits.Count -eq 0) {
            Write-Output ("  SCANMATRIX: no offset in any of the {0} block(s) carries a 4x4 whose" -f $sel.Count)
            Write-Output "  three columns are mutually orthogonal with a sane tangent pair. That is a"
            Write-Output "  SCOPED NEGATIVE, not a silent one - run -SelfTest to confirm the scanner"
            Write-Output "  itself still finds a planted matrix before believing it."
        } else {
            Write-Output "  offset|layout|tanH|tanV  x blocks   (R = row-major, T = transposed)"
            $hits.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 12 | ForEach-Object {
                $p = $_.Key -split '\|'
                $ar = if ($aspectH -gt 0) { " aspect {0:F4} (backbuffer {1:F4})" -f ([double]$p[2] / [double]$p[3]), ($aspectW / $aspectH) } else { "" }
                Write-Output ("    f{0,-4} {1}  tanH={2} tanV={3}  x{4}{5}" -f $p[0], $p[1], $p[2], $p[3], $_.Value, $ar)
            }
            Write-Output "  Believe a row only if it is at ONE offset with a plurality of blocks AND"
            Write-Output "  tanH/tanV matches the backbuffer aspect. Pass -Aspect WxH to check that here."
        }
    }
    return
}

# ---- -Diff: derive the layout with no layout assumption at all -------------
if ($Diff) {
    $fileA = @(Get-Item -Path $Path[0])[0]
    $fileB = @(Get-Item -Path $Diff)[0]
    Write-Output ""
    Write-Output "== DIFF: $($fileA.Name)  vs  $($fileB.Name) =="
    $a = Parse-Dump $fileA
    $b = Parse-Dump $fileB
    $selA = Select-Blocks $a
    $selB = Select-Blocks $b
    Report-Blocks "A $($fileA.Name)" $a $selA
    Report-Blocks "B $($fileB.Name)" $b $selB
    # Count-agnostic: the old hardcoded 336 both truncated large buffers and
    # ignored anything past float 335 that a mode-3 upload does carry.
    $nIdx = [math]::Max($a.maxFloats, $b.maxFloats)
    $ma = Block-Modes $selA $nIdx
    $mb = Block-Modes $selB $nIdx
    $expect = @()
    if ($DiffFovs.Count -eq 2) {
        $r = [math]::Tan($DiffFovs[0] * [math]::PI / 360.0) / [math]::Tan($DiffFovs[1] * [math]::PI / 360.0)
        # A projection MATRIX term is 1/tan, whose ratio is the reciprocal, and
        # sign-flipped slots are common (BS1's own block carries -tanH and
        # -2tanV), so all six forms are tagged rather than just three.
        $expect = @(@{ name = 'tan ratio'; v = $r }, @{ name = '1/tan ratio'; v = 1.0 / $r },
                    @{ name = '2x tan ratio'; v = 2.0 * $r }, @{ name = 'half tan ratio'; v = 0.5 * $r },
                    @{ name = '-tan ratio'; v = - $r }, @{ name = '-1/tan ratio'; v = -1.0 / $r })
        Write-Output ("FOV {0} vs {1}: tan(a/2)/tan(b/2) = {2:F6} (reciprocal {3:F6})" -f `
            $DiffFovs[0], $DiffFovs[1], $r, (1.0 / $r))
    }
    if ($DiffAspects -match '^(\d+)[xX](\d+)\s*,\s*(\d+)[xX](\d+)$') {
        $w1 = [double]$Matches[1]; $h1 = [double]$Matches[2]
        $w2 = [double]$Matches[3]; $h2 = [double]$Matches[4]
        $rv = ($h1 / $w1) / ($h2 / $w2)
        $expect += @(@{ name = 'aspect ratio (tanV moves, law A)'; v = $rv },
                     @{ name = '1/aspect ratio'; v = 1.0 / $rv },
                     @{ name = '2x aspect ratio'; v = 2.0 * $rv },
                     @{ name = 'PINNED (law A tanH / law B tanV)'; v = 1.0 })
        Write-Output ("ASPECT {0}x{1} vs {2}x{3}: (h1/w1)/(h2/w2) = {4:F6}" -f $w1, $h1, $w2, $h2, $rv)
        Write-Output ("  Under law A (option is a true horizontal) tanH is PINNED at ratio 1.0 and")
        Write-Output ("  tanV moves by {0:F4}; under law B the reverse. THE IDENTIFICATION IS THE" -f $rv)
        Write-Output ("  CROSS-PRODUCT with a -DiffFovs run: an index that MOVES under FOV and is")
        Write-Output ("  PINNED here is a horizontal projection term. Neither diff alone says that.")
    }
    Write-Output ""
    Write-Output "float indices whose modal value MOVED between the two dumps:"
    Write-Output "  (share = how many blocks hold the modal value; a low share means the mode is"
    Write-Output "   per-object noise rather than a view constant, which is the normal case on an"
    Write-Output "   engine with a per-object b0 - use -BlockBytes to pin one tier.)"
    $moved = 0; $pinned = 0; $skipped = 0
    for ($i = 0; $i -lt $nIdx; $i++) {
        if (-not $ma.ContainsKey($i) -or -not $mb.ContainsKey($i)) { continue }
        $shA = $ma[$i].n / [double]$selA.Count
        $shB = $mb[$i].n / [double]$selB.Count
        if ($MinModeShare -gt 0 -and ([math]::Min($shA, $shB) -lt $MinModeShare)) { $skipped++; continue }
        $va = $ma[$i].v; $vb = $mb[$i].v
        if ([math]::Abs($va - $vb) -le 0.0005) { $pinned++; continue }
        $moved++
        $ratio = if ([math]::Abs($vb) -gt 1e-9) { $va / $vb } else { [double]::NaN }
        $tag = ''
        foreach ($e in $expect) {
            if ($e.v -eq 1.0) { continue }  # PINNED is reported by the pinned pass, not here
            if (-not [double]::IsNaN($ratio) -and [math]::Abs($ratio - $e.v) -lt 0.002) {
                $tag = "  <== $($e.name) - PROJECTION TERM"
            }
        }
        Write-Output ("  f[{0,3}]  A={1,12:F4}  B={2,12:F4}  A/B={3,10:F4}  share {4,3:P0}/{5,3:P0}{6}" -f `
            $i, $va, $vb, $ratio, $shA, $shB, $tag)
    }
    Write-Output ""
    Write-Output ("{0} indices moved, {1} pinned, {2} skipped below -MinModeShare {3}." -f `
        $moved, $pinned, $skipped, $MinModeShare)
    Write-Output "A contiguous run of flagged indices IS the ray block; its first index is -RayOffset."
    if ($DiffAspects) {
        Write-Output "For the aspect axis, the PINNED indices are as informative as the moved ones -"
        Write-Output "intersect them with a -DiffFovs run before concluding anything."
    }
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
        $blocks = Select-Blocks $parsed
        Report-Blocks $file.Name $parsed $blocks
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
                Write-Output ("    #{0:D5} {1} a={2} b0={3} srv0=T{4} stk={5}" -f `
                    $_.idx, $_.kind, $_.a, $_.cb0b, $_.srv0, $stkHead)
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
