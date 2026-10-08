# lib/msvc.ps1 - where the MSVC toolset and Windows SDK are on THIS machine, for the
# host test runner (tools/host-test.ps1). Dot-source it and call Enter-BvrMsvcEnv.
#
# vswhere first (it finds the VS Build Tools under Program Files (x86) as well as a
# full Visual Studio), a glob as the fallback, and a clear error naming both. Ported
# from the Dishonored VR mod, where a hardcoded glob made every host suite throw on a
# Build-Tools-only machine.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
function Get-BvrMsvcRoot {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $root = $null
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
        if ($vs) {
            $root = (Get-ChildItem "$vs\VC\Tools\MSVC\*" -Directory -ErrorAction SilentlyContinue |
                     Sort-Object Name -Descending | Select-Object -First 1).FullName
        }
    }
    if (-not $root) {
        $root = (Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*" -Directory -ErrorAction SilentlyContinue |
                 Sort-Object Name -Descending | Select-Object -First 1).FullName
    }
    if (-not $root) {
        throw "MSVC toolset not found: neither vswhere ($vswhere) nor C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC has one. Install the VS Build Tools C++ workload."
    }
    return $root
}

# Sets INCLUDE/LIB for a 32-bit host build and returns what to restore, plus cl.exe.
function Enter-BvrMsvcEnv {
    $root = Get-BvrMsvcRoot
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
    $sdk = (Get-ChildItem "$kits\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    $lib = (Get-ChildItem "$kits\Lib" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    if (-not $sdk -or -not $lib) { throw "Windows 10/11 SDK not found under $kits" }
    $saved = @{ INCLUDE = $env:INCLUDE; LIB = $env:LIB }
    $env:INCLUDE = "$root\include;$sdk\ucrt;$sdk\shared;$sdk\um"
    $env:LIB = "$root\lib\x86;$lib\ucrt\x86;$lib\um\x86"
    return [pscustomobject]@{ Cl = "$root\bin\Hostx64\x86\cl.exe"; Saved = $saved }
}

function Exit-BvrMsvcEnv($State) {
    if (-not $State) { return }
    $env:INCLUDE = $State.Saved.INCLUDE
    $env:LIB = $State.Saved.LIB
}
