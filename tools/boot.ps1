# Boot BioShock Remastered into GAMEPLAY on the newest save: launch via Steam,
# dismiss the revert-Options 'Message' dialog with &No if it appears, foreground
# the game window (the boot sequence pauses unfocused), then press A every
# ~3.5 s through the menu until the mod logs the strict gameplay-view
# transition. Save-agnostic since session 19: the detector is the
# "[b1r] view state: GAMEPLAY" line (ShockPlayer vtable predicate, logged on
# transition by camera.cpp), not a hardwired save location - needs the
# session-19+ DLL installed. After detection a single B press closes the MAP
# screen the A-press loop sometimes leaves open (known harness trap);
# callers should still screenshot-verify before measuring.
$repo = Split-Path -Parent $PSScriptRoot
$log = "$env:LOCALAPPDATA\BioshockVR\bioshockvr.log"

Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@

Start-Process "steam://rungameid/409710"
"launched via steam"

# Phase 1: wait for the game process; dismiss any 'Message' dialog with &No.
$game = $null
for ($i = 0; $i -lt 120; $i++) {
    $dlg = [W]::FindWindow("#32770", "Message")
    if ($dlg -ne [IntPtr]::Zero) {
        $no = [W]::FindWindowEx($dlg, [IntPtr]::Zero, "Button", "&No")
        if ($no -ne [IntPtr]::Zero) {
            [W]::SendMessage($no, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            "dismissed revert-Options dialog with No"
        }
    }
    $p = Get-Process BioshockHD -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowTitle -like "*Bioshock*") { $game = $p; break }
    Start-Sleep -Milliseconds 1000
}
if (-not $game) { "FAIL: game window never appeared"; exit 1 }
"game window up (pid $($game.Id)), foregrounding"
[W]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
Start-Sleep -Seconds 3

# Phase 2: A-press loop until the strict gameplay-view transition logs. The
# line fires once per menu->gameplay transition, so search the whole log (it
# is truncated at DLL attach, small, and the one-shot line would scroll out
# of a fixed tail window between polls).
for ($i = 0; $i -lt 70; $i++) {
    $hit = Select-String -Path $log -Pattern "view state: GAMEPLAY" -SimpleMatch -Quiet -ErrorAction SilentlyContinue
    if ($hit) {
        "GAMEPLAY REACHED (after $i presses)"
        # The A-press loop can leave the MAP screen open; B closes it (a stray
        # B in gameplay is at most a jump on the test save).
        Start-Sleep -Milliseconds 1500
        & "$repo\tools\game-cmd.ps1" "vrinput test press B 250" | Out-Null
        exit 0
    }
    $p = Get-Process BioshockHD -ErrorAction SilentlyContinue
    if (-not $p) { "FAIL: game process died"; exit 1 }
    [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    & "$repo\tools\game-cmd.ps1" "vrinput test press A 250" | Out-Null
    Start-Sleep -Milliseconds 3500
}
"FAIL: gameplay never reached (timed out)"
exit 1
