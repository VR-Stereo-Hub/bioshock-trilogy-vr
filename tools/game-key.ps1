# game-key.ps1 - press a KEYBOARD key in the game window.
#
# The harness has never had a keyboard lane. `vrinput test press` composes
# XInput PAD buttons only, and docs/bioshock2/TESTING.md records the gap (and
# asks before adding one - permission given, session 35). Without this, every
# soak and every bisect step needs a human to press Space at the BS2 title
# screen, which is what makes those runs unattendable.
#
# SCANCODES, NOT VIRTUAL-KEY CODES. This is the whole point of the script.
# DirectInput and RawInput consumers read the hardware scancode and routinely
# ignore VK-only injection - which is the most likely reason session 24's
# synthetic clicks and Enter presses "were largely IGNORED" by BS2's gameswf
# menu. Injecting with KEYEVENTF_SCANCODE puts the same bytes on the wire a
# real keyboard would.
#
# keybd_event rather than SendInput deliberately: SendInput's INPUT union needs
# a different FieldOffset on x86 and x64, so a struct written for one silently
# corrupts on the other. keybd_event takes scalars, is a thin wrapper over
# SendInput on modern Windows, and injects at exactly the same level.
#
# Usage:
#   .\tools\game-key.ps1 -Game bs2 -Key Space
#   .\tools\game-key.ps1 -Game bs2 -Key Space -Repeat 3 -Delay 700
#   .\tools\game-key.ps1 -Game bs2 -Scan 0x39            # raw scancode escape hatch
[CmdletBinding()]
param(
    [ValidateSet("bs1", "bs2")][string]$Game = "bs1",
    [string]$Key = "",
    [int]$Scan = 0,
    [int]$Repeat = 1,
    [int]$Delay = 500,
    # Hold time. A frame-polled menu can miss a press that is too short.
    [int]$HoldMs = 60,
    [switch]$NoFocus
)

$ErrorActionPreference = 'Stop'

# Set 1 make codes. Extended keys would need KEYEVENTF_EXTENDEDKEY as well;
# none of the keys here are extended.
$scans = @{
    'space'  = 0x39; 'enter' = 0x1C; 'return' = 0x1C; 'escape' = 0x01; 'esc' = 0x01
    'tab'    = 0x0F; 'e'     = 0x12; 'f'      = 0x21; 'y'      = 0x15; 'n'   = 0x31
    'f9'     = 0x43; 'f10'   = 0x44; 'f11'    = 0x57; 'f12'    = 0x58
    '1'      = 0x02; '2'     = 0x03; '3'      = 0x04
}

if ($Scan -eq 0) {
    if (-not $Key) { throw "give -Key <name> or -Scan <code>" }
    $k = $Key.ToLower()
    if (-not $scans.ContainsKey($k)) {
        throw "unknown key '$Key'. Known: $(($scans.Keys | Sort-Object) -join ', '). " +
              "Or pass -Scan <set-1 make code>."
    }
    $Scan = $scans[$k]
}

Add-Type -ErrorAction SilentlyContinue @'
using System; using System.Runtime.InteropServices;
public static class BvrKey {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
'@

$proc = if ($Game -eq "bs2") { "Bioshock2HD" } else { "BioshockHD" }
# Pick the LIVE instance. Get-Process can return several entries: an exited
# process lingers as long as anything holds a handle to it (session 38 - a
# killed BS2 stayed listed for minutes and every -Key call died on
# "cannot convert System.Object[] to IntPtr", silently invalidating a whole
# unattended run). Drop exited entries, prefer one with a real window, and
# take the newest if several are genuinely alive.
$p = @(Get-Process $proc -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited })
if ($p.Count -gt 1) {
    $withWindow = @($p | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
    if ($withWindow.Count -ge 1) { $p = $withWindow }
    # Order by Id, not StartTime: StartTime throws "Access is denied" on
    # processes this session cannot query, and one throw kills the pipeline.
    $p = @($p | Sort-Object Id -Descending)
}
$p = $p | Select-Object -First 1
if (-not $p) { throw "$proc is not running" }

# The game must have focus or the injection lands on whatever does. In a headset
# session pass -NoFocus and accept that the press may go elsewhere - stealing
# focus drops the XR session out of FOCUSED (session 33).
if (-not $NoFocus -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
    [void][BvrKey]::ShowWindow($p.MainWindowHandle, 9)   # SW_RESTORE
    [void][BvrKey]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 400
}

$KEYEVENTF_SCANCODE = 0x0008
$KEYEVENTF_KEYUP    = 0x0002

for ($i = 0; $i -lt $Repeat; $i++) {
    [BvrKey]::keybd_event(0, [byte]$Scan, $KEYEVENTF_SCANCODE, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [BvrKey]::keybd_event(0, [byte]$Scan, $KEYEVENTF_SCANCODE -bor $KEYEVENTF_KEYUP, [UIntPtr]::Zero)
    if ($i -lt $Repeat - 1) { Start-Sleep -Milliseconds $Delay }
}

Write-Output ("pressed scancode 0x{0:X2} x{1} in {2}" -f $Scan, $Repeat, $proc)
