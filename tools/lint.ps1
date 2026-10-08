# lint.ps1 - the repo's static checks over every TRACKED file. Exit 0 = clean.
#
#   .\tools\lint.ps1            # run before a commit; fails on any FAIL line
#
# FAIL (each is an existing rule here, written down in CLAUDE.md or in the scripts):
#   - a .ps1/.bat that is not pure ASCII or starts with a BOM (PowerShell 5.1 misreads
#     BOM-less UTF-8; every script here says so in its NOTE line)
#   - a .ps1 that does not parse
#   - an IDAPython script (tools\ida\*.py) with a BOM (IDA refuses it)
#   - game-derived content tracked: decompiled script, packages, captures, dumps
#     (CLAUDE.md hard rule: NEVER commit game-derived content)
# NOTE only (not rules here yet - the Dishonored VR mod enforces them, this repo has not
# agreed to):
#   - em dashes (U+2014): PowerShell 5.1 parse errors and log mojibake were the reason there
# Ported from the Dishonored VR mod's lint.ps1.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
$fail = 0
$emNotes = 0
function Bad($m) { $script:fail++; Write-Host "LINT FAIL: $m" -ForegroundColor Red }
try {
    $tracked = @(& git ls-files)
    $emdash = [char]0x2014
    foreach ($f in $tracked) {
        if ($f -like 'third_party/*' -or $f -like 'docs/brvr-reference/*') { continue }
        $full = Join-Path $repo $f
        if (-not (Test-Path -LiteralPath $full)) { continue }   # deleted in the working tree
        if ($f -match '\.(ico|png|jpg|dll|exe|dmp|bin|zip)$') { continue }
        if ($f -match '\.(ps1|bat)$') {
            $bytes = [IO.File]::ReadAllBytes($full)
            if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { Bad "BOM in $f" }
            elseif ($bytes | Where-Object { $_ -gt 0x7F } | Select-Object -First 1) { Bad "non-ASCII byte in $f" }
            if ($f -match '\.ps1$') {
                $e = $null
                [void][System.Management.Automation.Language.Parser]::ParseFile($full, [ref]$null, [ref]$e)
                if ($e) { Bad "$f does not parse: $($e[0].Message) (line $($e[0].Extent.StartLineNumber))" }
            }
        }
        if ($f -match '^tools/ida/.*\.py$') {
            $bytes = [IO.File]::ReadAllBytes($full)
            if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { Bad "BOM in $f (IDA refuses it)" }
        }
        if (-not $Quiet -and ([IO.File]::ReadAllText($full)).Contains($emdash)) { $emNotes++ }
    }
    # Game-derived content. tools/uscript/README.md is the one tracked file there on purpose.
    $derived = @($tracked | Where-Object {
        ($_ -like 'tools/uscript/*' -and $_ -ne 'tools/uscript/README.md') -or
        $_ -match '\.(u|upk|bsm|blk|lbf|fsb|bik|xxx|psk|pskx|psa|tga|dds|rdc|dmp|i64|idb|swf|gfx|uc)$'
    })
    if ($derived.Count) { Bad "game-derived content tracked: $($derived -join ', ')" }
} finally { Pop-Location }
if ($emNotes -and -not $Quiet) { Write-Host "lint note: $emNotes tracked file(s) contain an em dash (not a rule in this repo)" }
if ($fail -eq 0) { Write-Host "lint: clean" -ForegroundColor Green; exit 0 }
exit 1
