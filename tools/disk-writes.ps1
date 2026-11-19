#!/usr/bin/env pwsh
<#
.SYNOPSIS
  What this machine writes to its drives over a window: bytes per physical disk, per volume, and the
  processes that asked to write the most. Read-only (roadmap T185).

.DESCRIPTION
  tools/disk-writes.ps1 [-Seconds 600] [-Interval 15] [-Top 15] [-Names <process names>] [-Out <file.json>]

  Samples the performance counters `\PhysicalDisk(*)\Disk Write Bytes/sec`,
  `\LogicalDisk(*)\Disk Write Bytes/sec` and `\Process(*)\IO Write Bytes/sec` every -Interval
  seconds for -Seconds, and prints megabytes written per drive (what wears it), per volume, the -Top
  processes summed by image name (cl#1, cl#2 ... add up under cl), and the -Names asked about. It
  needs no elevation and changes nothing: a counter read is all it does. What each counter counts,
  and what the process numbers miss (a process shorter than a sample, writes through a mapped
  file), is in tools/lib/DiskWrites.psm1; docs/ci/what-to-run.md, "What a build writes", has what a
  build costs the drive and how that was measured instead.

  -Out writes the same as JSON (seconds, samples, disks, volumes, processes in bytes, and the time),
  for a log kept per batch. The tool is not wired into the gate: whether it is is the owner's call.

  Exit codes: 0 done, 2 usage error.
#>
[CmdletBinding()]
param(
  [int]$Seconds = 600,
  [int]$Interval = 15,
  [int]$Top = 15,
  [string[]]$Names = @('cl', 'link', 'ccache', 'ninja', 'slangc', 'cmake', 'MsMpEng', 'SearchIndexer', 'nvcontainer', 'NVDisplay.Container', 'dwm'),
  [string]$Out
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/DiskWrites.psm1') -Force

if (-not $IsWindows) { Write-Host 'disk-writes: Windows only (performance counters)' -ForegroundColor Red; exit 2 }
if ($Interval -lt 1 -or $Seconds -lt $Interval) { Write-Host 'disk-writes: -Interval must be 1 or more and -Seconds at least one interval' -ForegroundColor Red; exit 2 }

$counters = @('\PhysicalDisk(*)\Disk Write Bytes/sec', '\LogicalDisk(*)\Disk Write Bytes/sec', '\Process(*)\IO Write Bytes/sec')
$count = [int][math]::Floor($Seconds / $Interval)
$started = Get-Date
Write-Host "disk-writes: $count samples of $Interval s from $($started.ToString('yyyy-MM-dd HH:mm:ss'))"
# A process that exits between two samples makes Get-Counter report an error for its instance; the
# sample set is still whole for everything else, so those are not failures.
$raw = @(Get-Counter -Counter $counters -SampleInterval $Interval -MaxSamples $count -ErrorAction SilentlyContinue)
$sets = @($raw | ForEach-Object {
    [pscustomobject]@{ Timestamp = $_.Timestamp; Samples = @($_.CounterSamples | ForEach-Object { [pscustomobject]@{ Path = $_.Path; CookedValue = $_.CookedValue } }) }
  })
$m = Measure-CounterWrites -Sets $sets -IntervalSeconds $Interval
Format-DiskWritesReport -Measure $m -Top $Top -Names $Names -Title "from $($started.ToString('yyyy-MM-dd HH:mm'))" | ForEach-Object { Write-Host $_ }

if ($Out) {
  $doc = [ordered]@{
    tool = 'disk-writes'; started = $started.ToString('o'); seconds = $m.Seconds; samples = $m.Samples; interval_s = $Interval
    disks = $m.Disks; volumes = $m.Volumes; processes = $m.Processes
  }
  $dir = Split-Path -Parent ([IO.Path]::GetFullPath($Out))
  if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  [IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), ($doc | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding $false))
  Write-Host "disk-writes: wrote $Out"
}
exit 0
