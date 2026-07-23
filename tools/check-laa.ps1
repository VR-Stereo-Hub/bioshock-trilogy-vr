# DR-2: reports whether BioshockHD.exe has the LARGEADDRESSAWARE flag
# (IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020 in the COFF Characteristics field).
# A 32-bit LAA process gets 4 GB of address space on 64-bit Windows, which is
# our headroom for stereo render targets. Read-only: this script never patches.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [string]$ExePath = "K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BioshockHD.exe"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath" }

$fs = [System.IO.File]::OpenRead($ExePath)
try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Seek(0x3C, "Begin") | Out-Null
    $peOffset = $br.ReadUInt32()
    $fs.Seek($peOffset, "Begin") | Out-Null
    if ($br.ReadUInt32() -ne 0x4550) { throw "Not a PE file." }
    $machine = $br.ReadUInt16()
    $fs.Seek($peOffset + 4 + 18, "Begin") | Out-Null   # COFF header Characteristics
    $characteristics = $br.ReadUInt16()
} finally {
    $fs.Close()
}

"Machine:          0x{0:X4} (0x014C = x86)" -f $machine
"Characteristics:  0x{0:X4}" -f $characteristics
if ($characteristics -band 0x0020) {
    "LARGEADDRESSAWARE: YES - 4 GB address space available."
} else {
    "LARGEADDRESSAWARE: NO - only 2 GB. Record this in docs/ENGINE_NOTES.md;"
    "consider patching the flag via the install script (with backup) when needed."
}
