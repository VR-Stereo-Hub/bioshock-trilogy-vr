# net-ping-watch.ps1 - ping the headset at a fixed rate during a session and write a CSV
# stamped on the mod log's own clock, so a link outage can be laid next to a stall or a
# frame gap in bioshockvr.log by timestamp.
#
#   .\tools\net-ping-watch.ps1 -Target 192.168.137.52 -Minutes 5
#   .\tools\net-ping-watch.ps1 -FromStreamer -Minutes 5     # finds the headset as Virtual Desktop's peer
#
# The question it answers: is a periodic hitch on a streamed headset DOWNSTREAM of the
# submit (streamer, encoder, Wi-Fi, headset) or on the PC? RTT spikes or losses on the
# same beat as the log's stalls put it on the link (a Wi-Fi scan, a power-save beacon);
# a flat trace puts it on the PC side. The Dishonored VR mod built it for a 5.66 s
# xrEndFrame stall on VDXR that never reproduced in the simulator.
#
# Output: %LOCALAPPDATA%\BioshockVR\pingwatch-<stamp>.csv with `time` in the log's
# HH:mm:ss.fff local-time form (bioshockvr.log prefixes every line `[HH:MM:SS.mmm]`),
# rtt in ms (-1 = no reply), and a summary with every spike and the intervals between
# them, so a regular beat is read off directly. Local output only; nothing is committed.
# Never launches anything. Ported from the Dishonored VR mod (its clock column was
# GetTickCount, which is what THAT log prints).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param(
    [string]$Target = "",
    [switch]$FromStreamer,
    [double]$Hz = 50,
    [double]$Minutes = 5,
    [int]$TimeoutMs = 200
)
$ErrorActionPreference = "Stop"

if ($FromStreamer) {
    $proc = Get-Process | Where-Object { $_.ProcessName -like "VirtualDesktop.Streamer*" } | Select-Object -First 1
    if (-not $proc) { throw "Virtual Desktop Streamer is not running; pass -Target <headset ip> instead." }
    $peers = @()
    try { $peers += Get-NetTCPConnection -OwningProcess $proc.Id -State Established -ErrorAction SilentlyContinue | ForEach-Object { $_.RemoteAddress } } catch {}
    try { $peers += Get-NetUDPEndpoint -OwningProcess $proc.Id -ErrorAction SilentlyContinue | ForEach-Object { $_.LocalAddress } } catch {}
    $peers = $peers | Where-Object { $_ -and $_ -ne "0.0.0.0" -and $_ -ne "::" -and $_ -notlike "127.*" } | Sort-Object -Unique
    $lan = $peers | Where-Object { $_ -like "192.168.*" -or $_ -like "10.*" -or $_ -like "172.1[6-9].*" -or $_ -like "172.2?.*" -or $_ -like "172.3[01].*" } | Select-Object -First 1
    if (-not $lan) { throw "No LAN peer on the streamer (seen: $($peers -join ', ')); pass -Target." }
    $Target = $lan
    Write-Host "pingwatch: headset = $Target (the streamer's peer)"
}
if (-not $Target) { throw "pass -Target <ip> or -FromStreamer" }

$dir = Join-Path $env:LOCALAPPDATA "BioshockVR"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$out = Join-Path $dir ("pingwatch-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".csv")
$ping = New-Object System.Net.NetworkInformation.Ping
$intervalMs = 1000.0 / $Hz
$n = [int]($Minutes * 60 * $Hz)
$sw = [Diagnostics.Stopwatch]::StartNew()
$rows = New-Object System.Collections.Generic.List[string]
$rtts = New-Object System.Collections.Generic.List[double]
$ms = New-Object System.Collections.Generic.List[double]
$stamps = New-Object System.Collections.Generic.List[string]
$rows.Add("time,elapsedMs,rttMs,status")
Write-Host ("pingwatch: {0} at {1} Hz for {2} min -> {3}" -f $Target, $Hz, $Minutes, $out)
for ($i = 0; $i -lt $n; $i++) {
    $due = $i * $intervalMs
    $wait = $due - $sw.Elapsed.TotalMilliseconds
    if ($wait -gt 1) { Start-Sleep -Milliseconds ([int]$wait) }
    $stamp = (Get-Date).ToString("HH:mm:ss.fff")   # the log's clock
    $t0 = $sw.Elapsed.TotalMilliseconds
    $rtt = -1.0; $status = "none"
    try {
        $r = $ping.Send($Target, $TimeoutMs)
        $status = [string]$r.Status
        if ($r.Status -eq "Success") { $rtt = [double]$r.RoundtripTime }
    } catch { $status = "error" }
    $rows.Add(("{0},{1:F1},{2},{3}" -f $stamp, $t0, $rtt, $status))
    $rtts.Add($rtt); $ms.Add($t0); $stamps.Add($stamp)
}
[IO.File]::WriteAllLines($out, $rows)

# Summary: the median, the tail, every spike (> 3x median or a loss) with its time, and
# the intervals between spikes - the number that either shows a regular beat or does not.
$ok = @($rtts | Where-Object { $_ -ge 0 } | Sort-Object)
if ($ok.Count -eq 0) { Write-Host "pingwatch: no replies at all from $Target"; exit 1 }
$median = $ok[[int]($ok.Count / 2)]
$p99 = $ok[[int]([math]::Min($ok.Count - 1, $ok.Count * 0.99))]
$spikeAt = [math]::Max(3.0 * $median, $median + 5)
$events = @(); $last = -1e12
for ($i = 0; $i -lt $rtts.Count; $i++) {
    if ($rtts[$i] -ge 0 -and $rtts[$i] -lt $spikeAt) { continue }
    if ($ms[$i] - $last -gt 200) { $events += ,@($ms[$i], $rtts[$i], $stamps[$i]) }   # one event per 200 ms
    $last = $ms[$i]
}
Write-Host ("pingwatch: {0} samples, {1} replies, median {2} ms, p99 {3} ms, max {4} ms, lost {5}" -f $rtts.Count, $ok.Count, $median, $p99, $ok[-1], ($rtts.Count - $ok.Count))
Write-Host ("pingwatch: {0} spike events (>= {1} ms or lost)" -f $events.Count, $spikeAt)
$prev = $null
foreach ($e in $events) {
    $gap = if ($prev -ne $null) { ("{0:F3} s after the previous" -f (($e[0] - $prev) / 1000.0)) } else { "first" }
    Write-Host ("  [{0}]  rtt {1}  {2}" -f $e[2], $e[1], $gap)
    $prev = $e[0]
}
if ($events.Count -ge 3) {
    $gaps = @(); for ($i = 1; $i -lt $events.Count; $i++) { $gaps += ($events[$i][0] - $events[$i-1][0]) / 1000.0 }
    $gs = $gaps | Sort-Object
    Write-Host ("pingwatch: spike intervals median {0:F3} s (min {1:F3}, max {2:F3})" -f $gs[[int]($gs.Count/2)], $gs[0], $gs[-1])
}
Write-Host "pingwatch: line the [time] column up against bioshockvr.log's [HH:MM:SS.mmm] prefix."
