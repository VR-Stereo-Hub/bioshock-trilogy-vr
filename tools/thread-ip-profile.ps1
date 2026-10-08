# thread-ip-profile.ps1 - bounded WOW64 instruction-pointer sampling of chosen threads.
# Not an on-CPU or stack profiler: each sample suspends ONE thread for microseconds,
# reads its EIP and resumes it inside a compiled finally. No engine writes, injection,
# symbols or work in the target. Never launches anything.
#
#   .\tools\thread-ip-profile.ps1 -Game bs1 -ThreadIds 1234,5678 -Samples 1000
#
# Pick the thread ids from thread-cpu-profile.ps1 first. Each sample is reported as
# module + RVA + 4 KB page, so a hot spot hands straight to tools\ida-run.ps1 or
# disasm-rva.py. Limits, printed into the output too: wall-time samples include waits;
# WOW64 kernel transitions can report thunk addresses. Run it from 64-bit PowerShell.
# Output: JSON (default %LOCALAPPDATA%\BioshockVR\profiles\ip-<game>-<stamp>.json).
# Ported from the Dishonored VR mod.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
[CmdletBinding()]
param([int]$TargetProcessId = 0,
      [ValidateSet("bs1", "bs2", "bsi")][string]$Game = "bs1",
      [Parameter(Mandatory)][uint32[]]$ThreadIds,
      [ValidateRange(1,2000)][int]$Samples = 1000,
      [string]$Out = "")
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 8) { throw 'Run with 64-bit PowerShell' }
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
    $Out = Join-Path $dir ("ip-$Game-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".json")
}
Add-Type @"
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
public static class BvrIpRead {
 // Windows SDK winnt.h WOW64_CONTEXT, control fields; full size includes x87/SSE.
 [StructLayout(LayoutKind.Explicit, Size=716)] public struct Context {
  [FieldOffset(0)] public uint Flags;
  [FieldOffset(180)] public uint Ebp;
  [FieldOffset(184)] public uint Eip;
  [FieldOffset(196)] public uint Esp;
 }
 public class Sample { public uint Tid, Eip; public int Error; public double PauseUs; }
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenThread(uint access,bool inherit,uint id);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")] static extern uint GetProcessIdOfThread(IntPtr h);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool IsWow64Process(IntPtr h,out bool wow);
 [DllImport("kernel32.dll",SetLastError=true)] static extern uint Wow64SuspendThread(IntPtr h);
 [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr h);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool Wow64GetThreadContext(IntPtr h,ref Context c);
 public static Sample[] Run(int pid,uint[] ids,int count) {
  using(var p=Process.GetProcessById(pid)) {
   bool wow;
   if(!IsWow64Process(p.Handle,out wow) || !wow) throw new Exception("Target must be WOW64");
   var handles=new IntPtr[ids.Length];
   var rows=new Sample[count*ids.Length];
   var random=new Random();
   try {
    for(int j=0;j<ids.Length;j++) {
     handles[j]=OpenThread(0x004A,false,ids[j]);
     if(handles[j]==IntPtr.Zero) throw new Win32Exception();
     if(GetProcessIdOfThread(handles[j])!=(uint)pid) throw new Exception("Thread owner mismatch");
    }
    for(int i=0;i<count;i++) {
     Thread.Sleep(random.Next(7,18));
     if(p.HasExited) throw new Exception("Target exited");
     for(int j=0;j<ids.Length;j++) {
      var row=new Sample(); row.Tid=ids[j];
      var context=new Context(); context.Flags=0x00010001;
      long start=Stopwatch.GetTimestamp();
      uint prior=Wow64SuspendThread(handles[j]);
      if(prior==UInt32.MaxValue) row.Error=Marshal.GetLastWin32Error();
      else {
       try {
        if(!Wow64GetThreadContext(handles[j],ref context)) row.Error=Marshal.GetLastWin32Error();
        else row.Eip=context.Eip;
       } finally {
        if(ResumeThread(handles[j])==UInt32.MaxValue)
         throw new Win32Exception(Marshal.GetLastWin32Error(),"ResumeThread failed; stop target immediately");
       }
      }
      row.PauseUs=(Stopwatch.GetTimestamp()-start)*1000000.0/Stopwatch.Frequency;
      rows[i*ids.Length+j]=row;
     }
    }
    return rows;
   } finally { foreach(var h in handles) if(h!=IntPtr.Zero) CloseHandle(h); }
  }
 }
}
"@
$p=Get-Process -Id $TargetProcessId
$identity=$p.StartTime.ToUniversalTime().ToString('o')
$modules=@($p.Modules | ForEach-Object {[pscustomobject]@{Name=$_.ModuleName;Path=$_.FileName;Base=$_.BaseAddress.ToInt64();Size=$_.ModuleMemorySize}})
$start=[DateTime]::UtcNow
$raw=[BvrIpRead]::Run($TargetProcessId,$ThreadIds,$Samples)
$end=[DateTime]::UtcNow
$rows=foreach($r in $raw){
 $module='unknown';$modulePath='unknown';$rva=[long]$r.Eip
 foreach($m in $modules){if($r.Eip -ge $m.Base -and $r.Eip -lt ($m.Base+$m.Size)){$module=$m.Name;$modulePath=$m.Path;$rva=$r.Eip-$m.Base;break}}
 [pscustomobject]@{ThreadId=$r.Tid;Module=$module;ModulePath=$modulePath;Rva=('0x{0:X}' -f $rva);Page=('0x{0:X}' -f ($rva -band -4096L));Error=$r.Error;PauseUs=$r.PauseUs}
}
[pscustomobject]@{ProcessId=$TargetProcessId;ProcessStartUtc=$identity;StartUtc=$start.ToString('o');EndUtc=$end.ToString('o');Limit='Perturbing wall-time IP samples include waits, not on-CPU percentages or call stacks. WOW64 kernel transitions can report thunk IPs.';Modules=$modules;Samples=@($rows)} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Out
Write-Output "wrote $Out"
$rows | Group-Object ThreadId,Module,Page | Sort-Object Count -Descending | Select-Object -First 16 Count,Name | Format-Table -AutoSize
$rows | Measure-Object PauseUs -Average -Maximum | Select-Object Average,Maximum
