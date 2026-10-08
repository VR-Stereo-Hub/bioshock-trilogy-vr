# thread-cpu-profile.ps1 - read-only per-thread CPU accounting of a running game.
# No elevation, suspension or injection; it never launches anything.
#
#   .\tools\thread-cpu-profile.ps1 -Game bs1 -Seconds 20          # the running BioshockHD.exe
#   .\tools\thread-cpu-profile.ps1 -TargetProcessId 1234 -Out x.json
#
# For every thread: CPU, user and kernel milliseconds over the window, its share of one
# core, its description and its START address as module+offset (which names threads the
# mod created - the present thread, the pacer - against the engine's own). Answers
# "which thread is the frame bound on" before anyone reaches for a profiler.
# Limits, printed into the output too: a start address is not a sampled stack, and CPU
# time says nothing about time spent BLOCKED (thread-ip-profile.ps1 samples that).
# Output: JSON (default %LOCALAPPDATA%\BioshockVR\profiles\threads-<game>-<stamp>.json).
# Ported from the Dishonored VR mod.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
[CmdletBinding()]
param([int]$TargetProcessId = 0,
      [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
      [ValidateRange(1,60)][int]$Seconds = 20,
      [string]$Out = "")
$ErrorActionPreference = 'Stop'
if (-not $TargetProcessId) {
    . (Join-Path $PSScriptRoot "lib\resolve-game-path.ps1")
    $exeName = [IO.Path]::GetFileNameWithoutExtension((Get-BvrGameSpec -Game $Game).Exe)
    $gp = @(Get-Process -Name $exeName -ErrorAction SilentlyContinue)
    if ($gp.Count -ne 1) { throw "expected one running $exeName, found $($gp.Count) - pass -TargetProcessId" }
    $TargetProcessId = $gp[0].Id
}
if (-not $Out) {
    $dir = Join-Path $env:LOCALAPPDATA "BioshockVR\profiles"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $Out = Join-Path $dir ("threads-$Game-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".json")
}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class BvrThreadRead {
 [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access,bool inherit,uint id);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h,out IntPtr desc);
 [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
 [DllImport("ntdll.dll")] public static extern int NtQueryInformationThread(IntPtr h,int cls,out IntPtr addr,int size,IntPtr len);
}
"@
function Snapshot {
 $p=Get-Process -Id $TargetProcessId -ErrorAction Stop
 $rows=@{}
 foreach($t in $p.Threads) {
  try { $rows[[string]$t.Id]=[pscustomobject]@{Cpu=$t.TotalProcessorTime.TotalMilliseconds;User=$t.UserProcessorTime.TotalMilliseconds;Kernel=$t.PrivilegedProcessorTime.TotalMilliseconds} } catch {}
 }
 return $rows
}
$p=Get-Process -Id $TargetProcessId
$startIdentity=$p.StartTime.ToUniversalTime().ToString('o')
$modules=@($p.Modules | ForEach-Object {[pscustomobject]@{Name=$_.ModuleName;Base=$_.BaseAddress.ToInt64();Size=$_.ModuleMemorySize}})
$before=Snapshot
$timer=[Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $Seconds
$after=Snapshot
$elapsed=$timer.Elapsed.TotalSeconds
$p=Get-Process -Id $TargetProcessId
if($p.StartTime.ToUniversalTime().ToString('o') -ne $startIdentity){throw 'Process identity changed'}
$rows=foreach($id in $after.Keys) {
 if(-not $before.ContainsKey($id)){continue}
 $name='';$start='unavailable';$h=[BvrThreadRead]::OpenThread(0x40,$false,[uint32]$id)
 if($h -ne [IntPtr]::Zero) {
  try {
   $desc=[IntPtr]::Zero
   if([BvrThreadRead]::GetThreadDescription($h,[ref]$desc) -eq 0 -and $desc -ne [IntPtr]::Zero){try{$name=[Runtime.InteropServices.Marshal]::PtrToStringUni($desc)}finally{[void][BvrThreadRead]::LocalFree($desc)}}
   $addr=[IntPtr]::Zero
   if([BvrThreadRead]::NtQueryInformationThread($h,9,[ref]$addr,[IntPtr]::Size,[IntPtr]::Zero) -eq 0){
    $v=$addr.ToInt64();$start=('0x{0:X}' -f $v)
    foreach($m in $modules){if($v -ge $m.Base -and $v -lt ($m.Base+$m.Size)){$start=('{0}+0x{1:X}' -f $m.Name,($v-$m.Base));break}}
   }
  } finally {[void][BvrThreadRead]::CloseHandle($h)}
 }
 $cpu=$after[$id].Cpu-$before[$id].Cpu
 [pscustomobject]@{ThreadId=$id;Description=$name;StartAddress=$start;CpuMs=$cpu;UserMs=$after[$id].User-$before[$id].User;KernelMs=$after[$id].Kernel-$before[$id].Kernel;OneCorePercent=100*$cpu/($elapsed*1000)}
}
$result=[pscustomobject]@{ProcessId=$TargetProcessId;ProcessStartUtc=$startIdentity;Seconds=$elapsed;CapturedUtc=[DateTime]::UtcNow.ToString('o');Limit='Thread start address is not a sampled executing stack; CPU time does not explain blocked time';Threads=@($rows|Sort-Object CpuMs -Descending)}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Out
Write-Output "wrote $Out"
$result.Threads | Select-Object -First 8 | Format-Table -AutoSize
