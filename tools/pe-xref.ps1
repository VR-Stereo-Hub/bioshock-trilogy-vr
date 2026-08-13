# Static cross-reference census over an arbitrary PE image. Read-only: this
# script never loads, never executes and never patches the file.
#
# WHY THIS EXISTS. "Zero callers on a function the engine must call every frame"
# is the single cheapest way to find out that a hook target is dead BEFORE
# installing the hook. It is the check that cracked BioShock 2 after a hook was
# installed that never fired, and it independently reproduced BioShock 1's
# "hook implementations, not exec thunks" on BioShock Infinite. Sessions 34 and
# earlier ran it from throwaway scratchpad scripts, which is exactly why those
# numbers could not be reproduced later. It lives in the repo now.
#
# The script is an ALGORITHM over a PE passed as a parameter - it embeds no game
# bytes, no game strings and no game addresses, so it is committable. Its OUTPUT
# is game-derived and must stay out of the repo; -OutFile refuses a path that
# resolves inside the working tree.
#
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
#
# Examples:
#   .\tools\pe-xref.ps1 -Exe "D:\...\BioShockInfinite.exe" -TargetRva 1E10C0,129280
#   .\tools\pe-xref.ps1 -Exe $exe -TargetRva 1E10C0 -FollowStubs -Mode Both
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    # Target RVAs in hex, with or without a leading 0x. Repeatable.
    [Parameter(Mandatory = $true)][string[]]$TargetRva,
    # Call   = E8 rel32 callers only (who CALLS this function).
    # AbsRef = 4-aligned dwords equal to ImageBase+rva (vtable slots, tables).
    [ValidateSet("Call", "AbsRef", "Both")][string]$Mode = "Both",
    # Also count callers routed through an E9 rel32 jump stub. A census that
    # silently misses stub-routed callers reports a FALSE ZERO, which is the
    # most expensive kind of wrong answer here. BS2 needed exactly this.
    [switch]$FollowStubs,
    [int]$ListCallers = 20,
    [string]$OutFile
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Exe)) { throw "Not found: $Exe" }

# The output carries game-derived addresses. Refuse to write it into the repo,
# in the same spirit as tools/lib/assert-no-conflict.ps1 guarding runtime.
if ($OutFile) {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    $outDir = Split-Path -Parent $OutFile
    if (-not $outDir) { $outDir = (Get-Location).Path }
    if (-not (Test-Path -LiteralPath $outDir)) {
        throw "-OutFile directory does not exist: $outDir"
    }
    $outFull = (Resolve-Path -LiteralPath $outDir).Path
    if ($outFull.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw ("-OutFile resolves inside the repo ($outFull). Caller lists are " +
               "game-derived content and must never be committed - write them to " +
               "the session scratchpad instead.")
    }
}

function Parse-Hex([string]$s) {
    $t = $s.Trim()
    if ($t.StartsWith("0x") -or $t.StartsWith("0X")) { $t = $t.Substring(2) }
    return [Convert]::ToUInt32($t, 16)
}

# ---- PE header parse --------------------------------------------------------

$bytes = [System.IO.File]::ReadAllBytes($Exe)
if ($bytes.Length -lt 0x40) { throw "Too small to be a PE." }
$peOff = [BitConverter]::ToUInt32($bytes, 0x3C)
if ($peOff + 0x18 -ge $bytes.Length) { throw "Bad e_lfanew." }
if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) { throw "Not a PE file." }

$machine      = [BitConverter]::ToUInt16($bytes, $peOff + 4)
$numSections  = [BitConverter]::ToUInt16($bytes, $peOff + 6)
$timeStamp    = [BitConverter]::ToUInt32($bytes, $peOff + 8)
$optHdrSize   = [BitConverter]::ToUInt16($bytes, $peOff + 20)
$optOff       = $peOff + 24
$optMagic     = [BitConverter]::ToUInt16($bytes, $optOff)
if ($optMagic -ne 0x10B) { throw ("Only PE32 is supported (magic 0x{0:X}). All three " +
                                  "BioShock titles are 32-bit." -f $optMagic) }
$sizeOfImage  = [BitConverter]::ToUInt32($bytes, $optOff + 56)
$imageBase    = [BitConverter]::ToUInt32($bytes, $optOff + 28)

# IMAGE_SECTION_HEADER is 40 bytes: 8 name, VirtualSize, VirtualAddress,
# SizeOfRawData, PointerToRawData, ..., Characteristics at +36.
$secOff = $optOff + $optHdrSize
$sections = @()
for ($i = 0; $i -lt $numSections; $i++) {
    $o = $secOff + $i * 40
    if ($o + 40 -gt $bytes.Length) { break }
    $nameBytes = $bytes[$o..($o + 7)]
    $name = ([System.Text.Encoding]::ASCII.GetString($nameBytes)).TrimEnd([char]0)
    $sections += [pscustomobject]@{
        Name       = $name
        VirtSize   = [BitConverter]::ToUInt32($bytes, $o + 8)
        Rva        = [BitConverter]::ToUInt32($bytes, $o + 12)
        RawSize    = [BitConverter]::ToUInt32($bytes, $o + 16)
        RawPtr     = [BitConverter]::ToUInt32($bytes, $o + 20)
        Chars      = [BitConverter]::ToUInt32($bytes, $o + 36)
    }
}
# IMAGE_SCN_MEM_EXECUTE = 0x20000000
$execSections = @($sections | Where-Object { ($_.Chars -band 0x20000000) -ne 0 })
if ($execSections.Count -eq 0) { throw "No executable section found." }

$targets = @()
foreach ($t in $TargetRva) { $targets += (Parse-Hex $t) }
$targetSet = @{}
foreach ($t in $targets) { $targetSet[$t] = $true }

function Rva-Section([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.Rva -and $rva -lt ($s.Rva + [Math]::Max($s.VirtSize, $s.RawSize))) {
            return $s
        }
    }
    return $null
}

# ---- pass 1: E8 rel32 callers ----------------------------------------------
#
# For every 0xE8 byte in an executable section, target = rva_of_next_insn +
# rel32. This is a NAIVE OPCODE SCAN, not a disassembly: a 0xE8 can also be an
# operand byte or immediate data, so the count is an upper bound. Requiring the
# computed target to land EXACTLY on a target RVA makes a false positive rare
# but not impossible, and the summary says so rather than pretending otherwise.

function Scan-Calls($wantSet) {
    $hits = @{}
    foreach ($k in $wantSet.Keys) { $hits[$k] = New-Object System.Collections.ArrayList }
    foreach ($s in $execSections) {
        $start = $s.RawPtr
        $len = [Math]::Min($s.RawSize, $bytes.Length - $start)
        if ($len -le 5) { continue }
        $end = $start + $len - 5
        for ($p = $start; $p -le $end; $p++) {
            if ($bytes[$p] -ne 0xE8) { continue }
            $rel = [BitConverter]::ToInt32($bytes, $p + 1)
            # RVA of the instruction after the 5-byte call.
            $nextRva = $s.Rva + ($p - $start) + 5
            $target = [int64]$nextRva + [int64]$rel
            if ($target -lt 0 -or $target -gt [uint32]::MaxValue) { continue }
            $t = [uint32]$target
            if (-not $wantSet.ContainsKey($t)) { continue }
            $callerRva = $s.Rva + ($p - $start)
            [void]$hits[$t].Add($callerRva)
        }
    }
    return $hits
}

# ---- pass 2: E9 rel32 jump stubs -------------------------------------------
# Incremental-link thunk tables route calls through a jmp. Find stubs landing on
# a target, then re-run the call pass against the stub RVAs themselves.

function Scan-Stubs($wantSet) {
    $stubs = @{}   # stubRva -> finalTargetRva
    foreach ($s in $execSections) {
        $start = $s.RawPtr
        $len = [Math]::Min($s.RawSize, $bytes.Length - $start)
        if ($len -le 5) { continue }
        $end = $start + $len - 5
        for ($p = $start; $p -le $end; $p++) {
            if ($bytes[$p] -ne 0xE9) { continue }
            $rel = [BitConverter]::ToInt32($bytes, $p + 1)
            $nextRva = $s.Rva + ($p - $start) + 5
            $target = [int64]$nextRva + [int64]$rel
            if ($target -lt 0 -or $target -gt [uint32]::MaxValue) { continue }
            $t = [uint32]$target
            if (-not $wantSet.ContainsKey($t)) { continue }
            $stubs[[uint32]($s.Rva + ($p - $start))] = $t
        }
    }
    return $stubs
}

# ---- pass 3: absolute dword references -------------------------------------
# Every 4-aligned dword equal to ImageBase+rva, histogrammed by section. This is
# the vtable detector: a virtually-dispatched function has FEW E8 callers and
# MANY .rdata dwords, and that inversion is itself the identification.

function Scan-AbsRefs($wantSet) {
    $want = @{}
    foreach ($k in $wantSet.Keys) { $want[[uint32]($imageBase + $k)] = $k }
    $hits = @{}
    foreach ($k in $wantSet.Keys) { $hits[$k] = @{} }
    foreach ($s in $sections) {
        $start = $s.RawPtr
        $len = [Math]::Min($s.RawSize, $bytes.Length - $start)
        if ($len -lt 4 -or $start -le 0) { continue }
        $end = $start + $len - 4
        for ($p = $start; $p -le $end; $p += 4) {
            $v = [BitConverter]::ToUInt32($bytes, $p)
            if (-not $want.ContainsKey($v)) { continue }
            $rva = $want[$v]
            if (-not $hits[$rva].ContainsKey($s.Name)) { $hits[$rva][$s.Name] = 0 }
            $hits[$rva][$s.Name] = $hits[$rva][$s.Name] + 1
        }
    }
    return $hits
}

# ---- run --------------------------------------------------------------------

$out = New-Object System.Collections.ArrayList
function Emit([string]$line) { [void]$out.Add($line); Write-Output $line }

Emit ("pe-xref: {0}" -f (Split-Path -Leaf $Exe))
Emit ("  machine 0x{0:X4}  ImageBase 0x{1:X8}  SizeOfImage 0x{2:X}  TimeDateStamp 0x{3:X8}" -f `
      $machine, $imageBase, $sizeOfImage, $timeStamp)
Emit ("  sections: " + (($sections | ForEach-Object {
        "{0}@0x{1:X}{2}" -f $_.Name, $_.Rva, $(if (($_.Chars -band 0x20000000) -ne 0) { "(X)" } else { "" })
      }) -join " "))
Emit ""

$callHits = $null
$stubMap = @{}
if ($Mode -eq "Call" -or $Mode -eq "Both") {
    $callHits = Scan-Calls $targetSet
    if ($FollowStubs) {
        $stubMap = Scan-Stubs $targetSet
        if ($stubMap.Count -gt 0) {
            $stubSet = @{}
            foreach ($k in $stubMap.Keys) { $stubSet[$k] = $true }
            $stubCallers = Scan-Calls $stubSet
            foreach ($stubRva in $stubMap.Keys) {
                $final = $stubMap[$stubRva]
                foreach ($c in $stubCallers[$stubRva]) { [void]$callHits[$final].Add($c) }
            }
        }
    }
}

$absHits = $null
if ($Mode -eq "AbsRef" -or $Mode -eq "Both") { $absHits = Scan-AbsRefs $targetSet }

foreach ($t in $targets) {
    $sec = Rva-Section $t
    $secName = if ($sec) { $sec.Name } else { "<outside any section>" }
    Emit ("RVA 0x{0:X}  (VA 0x{1:X8}, section {2})" -f $t, ($imageBase + $t), $secName)

    if ($null -ne $callHits) {
        $callers = @($callHits[$t] | Sort-Object -Unique)
        $stubCount = @($stubMap.Keys | Where-Object { $stubMap[$_] -eq $t }).Count
        Emit ("  E8 callers:      {0}{1}" -f $callers.Count,
              $(if ($FollowStubs) { "  (via $stubCount E9 stub(s))" } else { "" }))
        if ($callers.Count -gt 0 -and $ListCallers -gt 0) {
            $show = $callers | Select-Object -First $ListCallers
            Emit ("    " + (($show | ForEach-Object { "0x{0:X}" -f $_ }) -join ", ") +
                  $(if ($callers.Count -gt $ListCallers) { ", ... (+{0} more)" -f ($callers.Count - $ListCallers) } else { "" }))
        }
    }

    if ($null -ne $absHits) {
        $bySec = $absHits[$t]
        $total = 0
        foreach ($k in $bySec.Keys) { $total += $bySec[$k] }
        $detail = ($bySec.Keys | Sort-Object | ForEach-Object { "{0}={1}" -f $_, $bySec[$_] }) -join " "
        Emit ("  abs dword refs:  {0}   {1}" -f $total, $detail)
    }
    Emit ""
}

Emit "NOTE: the E8 pass is a naive opcode scan, not a disassembly - a 0xE8 that is"
Emit "actually an operand or immediate byte can produce a spurious caller. Requiring"
Emit "an exact landing makes that rare; treat a count as an upper bound. A count of"
Emit "ZERO is the reliable direction, and it is the one that matters: it means the"
Emit "dispatch is virtual, inlined or dynamic, and the target is not a hook seam."

if ($OutFile) {
    [System.IO.File]::WriteAllLines($OutFile, $out.ToArray())
    Write-Output ""
    Write-Output ("written: {0}" -f $OutFile)
}
