# Write one or more commands to the mod's command.txt seam, retrying past the
# transient share-lock when the game's 1 Hz poller has the file open.
# Foregrounds the game first: the poller runs inside the CalcView hook, which
# the engine pauses while the window is unfocused, so an unfocused game never
# reads the file.
# Usage: .\tools\game-cmd.ps1 "memscani 123" "memlist"
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Lines)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Cmd { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
'@
$p = Get-Process BioshockHD -ErrorAction SilentlyContinue
if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
    [Cmd]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 400
}
$cmd = "$env:LOCALAPPDATA\BioshockVR\command.txt"
$text = ($Lines -join "`n")
for ($i = 0; $i -lt 30; $i++) {
    try {
        [System.IO.File]::WriteAllText($cmd, $text + "`n")
        "wrote $($Lines.Count) command(s)"
        return
    } catch {
        Start-Sleep -Milliseconds 200
    }
}
throw "could not write command.txt after retries"
