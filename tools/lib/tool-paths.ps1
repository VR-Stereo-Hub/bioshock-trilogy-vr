# tool-paths.ps1 - where THIS machine keeps the offline tools (IDA, Blender, UModel, ...).
# Dot-source it from any script that needs one of them:
#   . (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
#   $idat = Get-BvrTool idat            # throws with the fix if it is not configured
#
# The answers live in a LOCAL, per-user preference file that is never committed:
#   $env:BVR_TOOLS_FILE                         (explicit override)
#   %LOCALAPPDATA%\BioshockVR\dev-tools.json    (default)
# Every contributor keeps their tools wherever they like; the repo knows only how
# to LOOK for a tool (the catalog below), and the file records what was FOUND or
# what the user SET. A "user" entry is never overwritten by detection.
# tools\tool-paths.ps1 is the command-line front end (-Init/-Set/-Get).
#
# Several of these tools are paid or third-party software: they are referenced by
# path, never copied into the tree. Never write a machine-specific path into a
# committed file - add a default-location rule to the catalog instead.
#
# Ported from the Dishonored VR mod (tools\lib\tool-paths.ps1 there); the catalog
# is per game here because one repo serves three.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8) and
# free of PS7-only syntax.

# Captured at dot-source time: inside a function or a catalog scriptblock,
# $PSScriptRoot/$PSCommandPath are not reliably this file's.
$script:BvrToolLibDir = $PSScriptRoot

function Get-BvrToolsFile {
    if ($env:BVR_TOOLS_FILE) { return $env:BVR_TOOLS_FILE }
    return Join-Path $env:LOCALAPPDATA "BioshockVR\dev-tools.json"
}

# The main checkout, also when called from a linked worktree: ignored tool folders
# (tools\uscript\_ueexplorer, build\model-tools) exist only there.
function Get-BvrMainCheckout {
    $repo = Split-Path -Parent (Split-Path -Parent $script:BvrToolLibDir)
    try {
        $common = (& git -C $repo rev-parse --path-format=absolute --git-common-dir 2>$null)
        if ($LASTEXITCODE -eq 0 -and $common) { return (Split-Path -Parent $common.Trim()) }
    } catch {}
    return $repo
}

# Highest-versioned match of a wildcard path, or $null.
function Find-BvrGlob {
    param([string[]]$Patterns)
    foreach ($p in $Patterns) {
        if (-not $p) { continue }
        $hits = @(Get-Item -Path $p -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)
        if ($hits.Count -gt 0) { return $hits[0].FullName }
    }
    return $null
}

function Find-BvrOnPath {
    param([string]$Exe)
    $c = Get-Command $Exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    return $null
}

# A game's binary folder through the same resolver every other script uses, or $null.
function Find-BvrGameDir {
    param([string]$Game)
    try {
        . (Join-Path $script:BvrToolLibDir 'resolve-game-path.ps1')
        return (Resolve-BvrGamePath -Game $Game -Quiet)
    } catch { return $null }
}

# The cooked content of a game, from its binary folder:
#   bs1/bs2  <game>\Build\Final         -> <game>\ContentBaked\pc   (.bsm/.blk packages)
#   bsi      <game>\Binaries\Win32      -> <game>\XGame\CookedPCConsole* (.xxx packages;
#            the folder name was not checked on a machine with Infinite installed)
function Find-BvrContentDir {
    param([string]$Game)
    $bin = Find-BvrGameDir $Game
    if (-not $bin) { return $null }
    $root = Split-Path -Parent (Split-Path -Parent $bin)
    if ($Game -eq 'bsi') {
        return (Find-BvrGlob @("$root\XGame\CookedPCConsole_FR", "$root\XGame\CookedPCConsole*"))
    }
    return (Find-BvrGlob @("$root\ContentBaked\pc"))
}

# The catalog: name -> what it is and where a default install puts it. Order is the
# display order. 'kind' is file or dir. 'create' makes a workspace dir on first use.
function Get-BvrToolCatalog {
    # Script scope, not locals: the find blocks run later, from the caller's scope.
    $script:pf = $env:ProgramFiles; $script:pf86 = ${env:ProgramFiles(x86)}
    $script:main = Get-BvrMainCheckout; $script:docs = [Environment]::GetFolderPath('MyDocuments')
    return [ordered]@{
        idat            = @{ kind='file'; group='IDA';     what='IDA headless binary (idat.exe; IDA 9 has one binary for 32 and 64 bit)'
                             find={ Find-BvrGlob @("$pf\IDA Professional *\idat.exe","$pf\IDA Pro*\idat.exe","$pf\IDA*\idat.exe","C:\IDA*\idat.exe") } }
        ida_workspace   = @{ kind='dir';  group='IDA';     what='staged exe copies + .i64 databases (bin\<game>\) and script outputs (out\); NEVER in the repo'
                             find={ Join-Path $env:LOCALAPPDATA "BioshockVR\ida" }; create=$true }
        blender         = @{ kind='file'; group='Models';  what='Blender (run headless: -b --python)'
                             find={ $x = Find-BvrGlob @("$pf\Blender Foundation\Blender *\blender.exe","$pf86\Steam\steamapps\common\Blender\blender.exe"); if ($x) { $x } else { Find-BvrOnPath blender.exe } } }
        psk_addon_zip   = @{ kind='file'; group='Models';  what='io_scene_psk_psa extension zip (PSK/PSA import/export for Blender)'
                             find={ Find-BvrGlob @("$main\build\model-tools\io_scene_psk_psa-*.zip","$docs\*\tools\io_scene_psk_psa-*.zip") } }
        umodel          = @{ kind='file'; group='Models';  what='UE Viewer / UModel (cooked packages -> PSK/PSA/glTF/TGA)'
                             find={ $x = Find-BvrGlob @("$main\build\model-tools\umodel.exe","$docs\*\tools\umodel.exe"); if ($x) { $x } else { Find-BvrOnPath umodel.exe } } }
        model_workspace = @{ kind='dir';  group='Models';  what='extracted meshes, .blend projects, exports, previews; NEVER in the repo'
                             find={ Join-Path $docs "BioshockVR-Models" }; create=$true }
        ffdec           = @{ kind='file'; group='Content'; what='JPEXS FFDec CLI (Scaleform .gfx/.swf -> XML, scripts, frames)'
                             find={ Find-BvrGlob @("$pf\FFDec\ffdec-cli.exe","$pf86\FFDec\ffdec-cli.exe","$main\build\model-tools\ffdec\ffdec-cli.exe") } }
        ueexplorer      = @{ kind='dir';  group='Content'; what='UE Explorer + ExportScripts.exe (tools\uscript-export, the UELib batch decompiler)'
                             find={ Find-BvrGlob @("$main\tools\uscript\_ueexplorer\ue-explorer","$main\tools\uscript\_ueexplorer") } }
        uscript_bs1     = @{ kind='dir';  group='Content'; what='BS1 decompiled UnrealScript (tools\uscript\bs1\, gitignored; tools\uscript-export.ps1)'
                             find={ Find-BvrGlob @("$main\tools\uscript\bs1") } }
        uscript_bs2     = @{ kind='dir';  group='Content'; what='BS2 decompiled UnrealScript (tools\uscript\bs2\, gitignored)'
                             find={ Find-BvrGlob @("$main\tools\uscript\bs2") } }
        uscript_bsi     = @{ kind='dir';  group='Content'; what='Infinite decompiled UnrealScript (tools\uscript\bsi\, gitignored)'
                             find={ Find-BvrGlob @("$main\tools\uscript\bsi") } }
        game_bs1        = @{ kind='dir';  group='Game';    what='folder holding BioshockHD.exe (lib\resolve-game-path.ps1)'
                             find={ Find-BvrGameDir bs1 } }
        game_bs2        = @{ kind='dir';  group='Game';    what='folder holding Bioshock2HD.exe'
                             find={ Find-BvrGameDir bs2 } }
        game_bsi        = @{ kind='dir';  group='Game';    what='folder holding BioShockInfinite.exe'
                             find={ Find-BvrGameDir bsi } }
        content_bs1     = @{ kind='dir';  group='Game';    what='BS1 ContentBaked\pc (the packages UModel reads)'
                             find={ Find-BvrContentDir bs1 } }
        content_bs2     = @{ kind='dir';  group='Game';    what='BS2 ContentBaked\pc'
                             find={ Find-BvrContentDir bs2 } }
        content_bsi     = @{ kind='dir';  group='Game';    what='Infinite XGame\CookedPCConsole_FR'
                             find={ Find-BvrContentDir bsi } }
        python          = @{ kind='file'; group='Runtime'; what='Python 3 for tools\*.py (capstone for disasm-rva.py, pefile for ue3-natives.py)'
                             find={ $x = Find-BvrOnPath py.exe; if ($x) { $x } else { Find-BvrOnPath python.exe } } }
        java            = @{ kind='file'; group='Runtime'; what='Java (ffdec.jar fallback)'
                             find={ Find-BvrOnPath java.exe } }
        renderdoc       = @{ kind='file'; group='Debug';   what='RenderDoc - all three games are D3D11, so it captures the GAME frame too'
                             find={ Find-BvrGlob @("$pf\RenderDoc\qrenderdoc.exe") } }
        pix             = @{ kind='file'; group='Debug';   what='PIX on Windows (D3D12-first; RenderDoc is the D3D11 tool here)'
                             find={ Find-BvrGlob @("$pf\Microsoft PIX\*\WinPix.exe") } }
        cheatengine     = @{ kind='file'; group='Debug';   what='Cheat Engine (live memory scan / structure dissect; a lead, not a finding)'
                             find={ Find-BvrGlob @("$pf\Cheat Engine*\Cheat Engine.exe","$pf86\Cheat Engine*\Cheat Engine.exe") } }
        hxd             = @{ kind='file'; group='Debug';   what='HxD hex editor'
                             find={ Find-BvrGlob @("$pf\HxD\HxD.exe","$pf86\HxD\HxD.exe") } }
        x32dbg          = @{ kind='file'; group='Debug';   what='x64dbg suite, 32-bit debugger (optional)'
                             find={ $x = Find-BvrGlob @("$pf\x64dbg\release\x32\x32dbg.exe","C:\x64dbg\release\x32\x32dbg.exe"); if ($x) { $x } else { Find-BvrOnPath x32dbg.exe } } }
        procdump        = @{ kind='file'; group='Debug';   what='Sysinternals ProcDump, 32-bit procdump.exe (tools\watch-crashes.ps1)'
                             find={ $x = Find-BvrGlob @("$pf\Sysinternals\procdump.exe","$pf86\Sysinternals\procdump.exe","C:\Sysinternals*\procdump.exe","C:\Tools\Sysinternals*\procdump.exe"); if ($x) { $x } else { Find-BvrOnPath procdump.exe } } }
        ghidra          = @{ kind='dir';  group='Debug';   what='Ghidra (optional free alternative to IDA)'
                             find={ Find-BvrGlob @("$pf\ghidra*","C:\ghidra*") } }
    }
}

function Read-BvrToolsFile {
    $f = Get-BvrToolsFile
    $map = [ordered]@{}
    if (Test-Path -LiteralPath $f) {
        $raw = Get-Content -LiteralPath $f -Raw
        if ($raw -and $raw.Trim()) {
            $j = $raw | ConvertFrom-Json
            if ($j.tools) { foreach ($p in $j.tools.PSObject.Properties) { $map[$p.Name] = $p.Value } }
        }
    }
    return $map
}

function Write-BvrToolsFile {
    param([System.Collections.IDictionary]$Map)
    $f = Get-BvrToolsFile
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $f) | Out-Null
    $doc = [ordered]@{
        schema  = 1
        comment = 'Local tool locations for the BioShock trilogy VR repo. Per user, never committed. Edit with tools\tool-paths.ps1 -Set name=path; source=user entries are never re-detected.'
        updated = (Get-Date -Format s)
        tools   = $Map
    }
    # UTF-8 without BOM so Python's json and IDA/Blender scripts read it cleanly.
    [IO.File]::WriteAllText($f, ($doc | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
}

# The path for one tool, or throw with the exact fix. -Optional returns $null instead.
function Get-BvrTool {
    param([Parameter(Mandatory)][string]$Name, [switch]$Optional)
    $map = Read-BvrToolsFile
    $cat = Get-BvrToolCatalog
    $p = $null
    if ($map.Contains($Name) -and $map[$Name].path) { $p = [string]$map[$Name].path }
    elseif ($cat.Contains($Name)) { $p = & $cat[$Name].find }
    if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    if ($p -and $cat.Contains($Name) -and $cat[$Name].create) {
        New-Item -ItemType Directory -Force -Path $p | Out-Null
        return $p
    }
    if ($Optional) { return $null }
    $why = if ($p) { "configured as '$p' but that path does not exist" } else { 'not configured and not found in any default location' }
    throw "Tool '$Name' $why. Fix: .\tools\tool-paths.ps1 -Set $Name=<path>   (file: $(Get-BvrToolsFile))"
}
