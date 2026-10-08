# blender-run.ps1 - run a Blender Python script headless (no window, no game).
#
#   .\tools\blender-run.ps1 -Setup                         # install/verify the PSK/PSA add-on, print versions
#   .\tools\blender-run.ps1 tools\blender\inspect_model.py -- --mesh <file.psk|.pskx|.glb|.obj> --out report.json
#   .\tools\blender-run.ps1 tools\blender\export_model.py -Blend <file.blend> -- --object Arms --out <file.obj|.psk|.psa|.glb|.fbx>
#
# Arguments after `--` go to the script (sys.argv after "--"). A relative --out lands in
# <model_workspace>\verification (inspect) or \exports (export). The script sees
# BVR_MODEL_WS (the workspace) in its environment. Exit code is the script's:
# --python-exit-code 1 makes an uncaught Python exception exit 1 instead of Blender's 0.
# NOT --factory-startup: that would disable the PSK/PSA add-on the workflow depends on.
# Never launches a game. Ported from the Dishonored VR mod.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [Parameter(Position=0)][string]$Script = "",
    [string]$Blend = "",
    [switch]$Setup,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$Rest = @()
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot "lib\tool-paths.ps1")
$blender = Get-BvrTool blender
$ws = Get-BvrTool model_workspace
$env:BVR_MODEL_WS = $ws
# Native tools write progress to stderr; under 'Stop' PowerShell 5.1 turns each line
# into a terminating error. Every native call below checks $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'

$probe = @'
import bpy, sys
ok = True
for name in ("psk.import_file", "psk.export", "psa.import_file", "psa.export"):
    ns, op = name.split(".")
    try:
        getattr(getattr(bpy.ops, ns), op).get_rna_type()
        print("BVR_PROBE op %s present" % name)
    except Exception as e:
        ok = False
        print("BVR_PROBE op %s MISSING (%s)" % (name, e))
print("BVR_PROBE blender %s" % bpy.app.version_string)
sys.exit(0 if ok else 1)
'@

if ($Setup) {
    $tmp = Join-Path $env:TEMP "bvr-blender-probe.py"
    [IO.File]::WriteAllText($tmp, $probe, (New-Object Text.UTF8Encoding($false)))
    # Note: hasattr(bpy.ops.psk, ...) is True for ANY name; only get_rna_type() can fail.
    & $blender -b --python-exit-code 1 --python $tmp 2>&1 | Where-Object { "$_" -like 'BVR_PROBE*' } | Write-Output
    if ($LASTEXITCODE -ne 0) {
        $zip = Get-BvrTool psk_addon_zip -Optional
        if (-not $zip) { throw "PSK/PSA add-on missing and no psk_addon_zip configured. Get io_scene_psk_psa from https://extensions.blender.org/add-ons/io-scene-psk-psa/ (ask first), then: .\tools\tool-paths.ps1 -Set psk_addon_zip=<zip>" }
        Write-Output "installing $zip into Blender's user_default extension repository"
        & $blender --command extension install-file -r user_default -e $zip 2>&1 | Write-Output
        & $blender -b --python-exit-code 1 --python $tmp 2>&1 | Where-Object { "$_" -like 'BVR_PROBE*' } | Write-Output
        if ($LASTEXITCODE -ne 0) { throw "PSK/PSA add-on still not registered after install" }
    }
    Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
    Write-Output "model workspace: $ws"
    if (-not $Script) { exit 0 }
}

if (-not $Script) { throw "Name a script (tools\blender\*.py) or pass -Setup. See docs\MODEL_WORKFLOW.md." }
$scriptPath = (Resolve-Path -LiteralPath $Script).Path
$argList = @('-b')
if ($Blend) { $argList += (Resolve-Path -LiteralPath $Blend).Path }
$argList += @('--python-exit-code', '1', '--python', $scriptPath)
$pass = @($Rest | Where-Object { $_ -ne '--' })
if ($pass.Count) { $argList += '--'; $argList += $pass }
Write-Output "blender $($argList -join ' ')"
& $blender @argList 2>&1 | ForEach-Object { "$_" } | Write-Output
exit $LASTEXITCODE
