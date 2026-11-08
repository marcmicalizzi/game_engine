<#
.SYNOPSIS
  Summarises a startup trace (`engine-view --startup-trace`, Chrome trace JSON) as tables.

.DESCRIPTION
  Prints, for one trace written by `profiling::write_chrome_trace` (docs/subsystems/profiling.md,
  "The startup trace"):
    - the events the trace dropped (a full buffer), the last event's end, and the peak working set
      and private bytes the sampler saw;
    - every stage name aggregated: how many, their summed wall and CPU seconds, and the process's
      reads over them (all threads, so overlapping stages count a read more than once);
    - the longest stages (per-mesh stages left out, they are in the aggregate), in time order, with
      their thread, CPU and reads, and the working set and private GiB at their start and end;
    - each thread's share of one stage name (`-ByThread`, `mesh` by default): how many it ran, when
      it first started and last ended — which is how a stage that ran on one thread while the pool
      slept shows itself;
    - the milestones.
  Written for T73 (docs/experiments/load-v35-2026-11-08.md). The repository takes no Python.

.EXAMPLE
  tools/startup-trace-summary.ps1 -Trace out/startup.json -Top 30
#>
param(
  [Parameter(Mandatory = $true)][string]$Trace,
  [int]$Top = 30,
  [string]$ByThread = 'mesh',
  [string[]]$PerItem = @('mesh', 'cluster read', 'generator inputs', 'source hash')
)
$ErrorActionPreference = 'Stop'
$j = [IO.File]::ReadAllText($Trace) | ConvertFrom-Json
$ev = $j.traceEvents
"dropped events: $($j.otherData.dropped)"
$names = @{}
$ev | Where-Object { $_.ph -eq 'M' } | ForEach-Object { $names[[int]$_.tid] = $_.args.name }
$mem = @($ev | Where-Object { $_.ph -eq 'C' -and $_.name -eq 'memory MiB' } | Sort-Object ts)
$memTs = [double[]]($mem | ForEach-Object { $_.ts })
function Get-MemAt([double]$ts) {
  if ($mem.Count -eq 0) { return '   n/a' }
  $i = [Array]::BinarySearch($memTs, $ts); if ($i -lt 0) { $i = (-bnot $i) - 1 }; if ($i -lt 0) { $i = 0 }
  $m = $mem[$i]; '{0,5:N1}/{1,5:N1}' -f ($m.args.working_set / 1024), ($m.args.private / 1024)
}
$x = @($ev | Where-Object { $_.ph -eq 'X' })
'last event end: {0:N1} s' -f (($x | ForEach-Object { $_.ts + $_.dur } | Measure-Object -Maximum).Maximum / 1e6)
if ($mem.Count -gt 0) {
  'peak working set {0:N2} GiB, peak private {1:N2} GiB' -f (($mem | ForEach-Object { $_.args.working_set } | Measure-Object -Maximum).Maximum / 1024),
    (($mem | ForEach-Object { $_.args.private } | Measure-Object -Maximum).Maximum / 1024)
}
''
'--- stages by name: count, summed wall s, summed CPU s, process reads GB over them'
$x | Group-Object name | ForEach-Object {
  [pscustomobject]@{
    name = $_.Name; n = $_.Count
    wall_s = [math]::Round((($_.Group | Measure-Object dur -Sum).Sum / 1e6), 1)
    cpu_s = [math]::Round((($_.Group | ForEach-Object { $_.args.cpu_ns } | Measure-Object -Sum).Sum / 1e9), 1)
    read_gb = [math]::Round((($_.Group | ForEach-Object { $_.args.read_bytes } | Measure-Object -Sum).Sum / 1e9), 2)
  }
} | Sort-Object wall_s -Descending | Format-Table -AutoSize | Out-String -Width 200
"--- the $Top longest stages, in time order: start s, wall s, thread, CPU s, read GB, WS/private GiB at start -> end"
$x | Where-Object { $_.name -notin $PerItem } | Sort-Object dur -Descending | Select-Object -First $Top | Sort-Object ts | ForEach-Object {
  $d = [string]$_.args.detail
  '{0,7:N1} {1,7:N1} {2,-9} {3,6:N1} {4,6:N2} {5} -> {6}  {7} {8}' -f ($_.ts / 1e6), ($_.dur / 1e6), $names[[int]$_.tid],
    ($_.args.cpu_ns / 1e9), ($_.args.read_bytes / 1e9), (Get-MemAt $_.ts), (Get-MemAt ($_.ts + $_.dur)), $_.name,
    $d.Substring(0, [math]::Min(60, $d.Length))
}
''
"--- '$ByThread' by thread: count, first start s, last end s, summed wall s"
$x | Where-Object { $_.name -eq $ByThread } | Group-Object tid | Sort-Object { [int]$_.Name } | ForEach-Object {
  $g = $_.Group
  '{0,-9} {1,6} {2,8:N1} {3,8:N1} {4,8:N1}' -f $names[[int]$_.Name], $g.Count, (($g.ts | Measure-Object -Minimum).Minimum / 1e6),
    (($g | ForEach-Object { $_.ts + $_.dur } | Measure-Object -Maximum).Maximum / 1e6), (($g.dur | Measure-Object -Sum).Sum / 1e6)
}
''
'--- milestones'
$ev | Where-Object { $_.ph -eq 'i' } | ForEach-Object {
  '{0,7:N1} s  {1,-24} WS {2,7:N0} MiB  private {3,7:N0} MiB  read {4,7:N0} MiB' -f ($_.ts / 1e6), $_.name, $_.args.working_set_mib, $_.args.private_mib, $_.args.read_mib
}
