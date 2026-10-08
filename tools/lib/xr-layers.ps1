# xr-layers.ps1 - opt THIS process out of implicit OpenXR API layers that cannot load
# into a 32-bit client, before a tool (xr_hello32, armcap32) starts one.
#
#   . (Join-Path $PSScriptRoot "lib\xr-layers.ps1")
#   $restore = Disable-BvrUnloadableXrLayers      # sets each bad layer's own opt-out env var
#   try { & $client } finally { Restore-BvrXrLayerEnv $restore }
#
# The mod does this for itself (src/core/vr/apilayer_guard.cpp, measured 2026-09-07: an
# OBS mirror layer registered x64-only took xrCreateInstance down with -32 for every
# runtime). The standalone 32-bit clients have no such guard, and on this dev PC the
# simulator self-test failed with "Failed loading layer information" for exactly that
# reason (2026-10-07). Same rules as the mod: read the implicit-layer registry keys, read
# each enabled layer's manifest, resolve its library, read the PE machine; a library that
# is not x86 gets the layer's DECLARED disable_environment variable set in this process
# only. The registry is never written, and the layer keeps working for 64-bit apps.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).

function Get-BvrPeMachine([string]$Path) {
    try {
        $fs = [IO.File]::OpenRead($Path)
        try {
            $br = New-Object IO.BinaryReader($fs)
            $fs.Seek(0x3C, 'Begin') | Out-Null
            $pe = $br.ReadInt32()
            $fs.Seek($pe, 'Begin') | Out-Null
            if ($br.ReadUInt32() -ne 0x00004550) { return $null }
            return $br.ReadUInt16()
        } finally { $fs.Close() }
    } catch { return $null }
}

# Returns @{ <var> = <previous value or $null> } for Restore-BvrXrLayerEnv.
function Disable-BvrUnloadableXrLayers([switch]$Quiet) {
    $saved = @{}
    $keys = @(
        'Registry::HKEY_CURRENT_USER\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit',
        'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit',
        'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
    )
    foreach ($k in $keys) {
        if (-not (Test-Path $k)) { continue }
        $props = Get-ItemProperty -Path $k
        foreach ($p in $props.PSObject.Properties) {
            if ($p.Name -like 'PS*') { continue }
            if ([int]$p.Value -ne 0) { continue }            # nonzero = disabled by the registry already
            $manifest = $p.Name
            if (-not (Test-Path -LiteralPath $manifest)) { continue }
            try { $j = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json } catch { continue }
            $layer = $j.api_layer
            if (-not $layer) { continue }
            $lib = [string]$layer.library_path
            if (-not [IO.Path]::IsPathRooted($lib)) { $lib = Join-Path (Split-Path -Parent $manifest) $lib }
            $machine = Get-BvrPeMachine $lib
            if ($null -eq $machine -or $machine -eq 0x014C) { continue }   # loadable, or unreadable: leave it
            $var = [string]$layer.disable_environment
            if (-not $var) {
                if (-not $Quiet) { Write-Warning ("xr layer '{0}' is {1:X4}, cannot load in 32-bit, and declares no disable_environment - xrCreateInstance will fail" -f $layer.name, $machine) }
                continue
            }
            if (-not $saved.ContainsKey($var)) { $saved[$var] = [Environment]::GetEnvironmentVariable($var) }
            [Environment]::SetEnvironmentVariable($var, '1')
            if (-not $Quiet) { Write-Host ("xr layer '{0}' is machine 0x{1:X4} (cannot load in a 32-bit client): set {2}=1 for this process" -f $layer.name, $machine, $var) }
        }
    }
    return $saved
}

function Restore-BvrXrLayerEnv([hashtable]$Saved) {
    if (-not $Saved) { return }
    foreach ($var in $Saved.Keys) { [Environment]::SetEnvironmentVariable($var, $Saved[$var]) }
}
