# The arithmetic of tools/disk-writes.ps1, as functions over counter samples so it is tested with no
# performance counter at all (tools/disk-writes.Tests.ps1, CTest `tools.disk_writes`). The script
# reads the counters; everything it adds up and prints is decided here. See docs/ci/what-to-run.md,
# "What a build writes".
#
# What the counters are, and so what the report can and cannot say:
# - `\PhysicalDisk(*)\Disk Write Bytes/sec` is what reached each drive: the number that wears it. A
#   volume striped over several drives shows as one instance per drive, with no letter.
# - `\LogicalDisk(*)\Disk Write Bytes/sec` is the same per volume, so a striped volume has a letter.
# - `\Process(*)\IO Write Bytes/sec` counts every write a process makes, to files, pipes and the
#   network alike, as the process asked for it (before the file cache: a temporary file deleted
#   before it was flushed counts here and never reaches a disk), and it does not count writes
#   through a mapped file, which the system's page writer makes later. A process that lived less
#   than one sample (a compiler, ccache) is missed entirely; a build's own total needs a job
#   object's accounting instead, which is how docs/ci/what-to-run.md measured it.
# Each counter is a rate averaged over the sample's interval, so the bytes of one sample are the rate
# times the seconds since the sample before it.

Set-StrictMode -Version Latest

# A counter path as Get-Counter writes it, `\\host\process(cl#3)\io write bytes/sec`, or without the
# host, `\physicaldisk(4 c:)\disk write bytes/sec`: Object, Instance ('' for none) and Counter, all
# lower case. $null for anything else. An instance may hold parentheses of its own ("foo (x86)"), so
# it runs to the last ")\" of the path.
function ConvertFrom-CounterPath([string]$Path) {
  if ("$Path" -notmatch '^(?:\\\\[^\\]+)?\\(?<object>[^\\(]+?)(?:\((?<instance>.*)\))?\\(?<counter>[^\\]+)$') { return $null }
  $instance = if ($Matches.ContainsKey('instance')) { $Matches['instance'] } else { '' }
  return [pscustomobject]@{
    Object = $Matches['object'].ToLowerInvariant(); Instance = "$instance".ToLowerInvariant(); Counter = $Matches['counter'].ToLowerInvariant()
  }
}

# Windows numbers the second and later processes of one image `cl#1`, `cl#2`: the report adds them up
# under the image's name, since a build's compilers come and go under new numbers all the time.
function Get-ProcessBaseName([string]$Instance) { return ($Instance -replace '#\d+$', '') }

# Bytes written over the window, per physical disk, per volume and per process name.
# -Sets: one object per sample, with Timestamp ([datetime]) and Samples (objects with Path and
#   CookedValue, bytes per second), as Get-Counter's sample sets have them.
# -IntervalSeconds: what a sample stands for when it has no earlier sample to measure from (the
#   first one), and the floor when two timestamps are closer than that.
# Returns Seconds (the window), Samples, Disks, Volumes, Processes (each an ordered map name -> bytes,
# largest first; `_total` and the idle process left out).
function Measure-CounterWrites {
  param([object[]]$Sets, [double]$IntervalSeconds = 15)
  $disks = @{}; $volumes = @{}; $procs = @{}
  $seconds = 0.0
  $previous = $null
  foreach ($set in @($Sets)) {
    $dt = $IntervalSeconds
    if ($null -ne $previous) {
      $gap = ($set.Timestamp - $previous).TotalSeconds
      if ($gap -gt $dt) { $dt = $gap }
    }
    $previous = $set.Timestamp
    $seconds += $dt
    foreach ($s in @($set.Samples)) {
      $p = ConvertFrom-CounterPath $s.Path
      if (-not $p -or $p.Instance -eq '_total') { continue }
      $bytes = [double]$s.CookedValue * $dt
      if ($bytes -le 0) { continue }
      switch -Regex ($p.Object) {
        '^physicaldisk$' { if ($p.Counter -eq 'disk write bytes/sec') { $disks[$p.Instance] = $disks[$p.Instance] + $bytes } }
        '^logicaldisk$' { if ($p.Counter -eq 'disk write bytes/sec') { $volumes[$p.Instance] = $volumes[$p.Instance] + $bytes } }
        '^process$' {
          if ($p.Counter -eq 'io write bytes/sec' -and $p.Instance -ne 'idle') {
            $name = Get-ProcessBaseName $p.Instance
            $procs[$name] = $procs[$name] + $bytes
          }
        }
      }
    }
  }
  $sorted = { param($h) $o = [ordered]@{}; foreach ($k in ($h.Keys | Sort-Object { -$h[$_] }, { $_ })) { $o[$k] = [math]::Round($h[$k]) }; $o }
  return [pscustomobject]@{
    Seconds = [math]::Round($seconds, 1); Samples = @($Sets).Count
    Disks = & $sorted $disks; Volumes = & $sorted $volumes; Processes = & $sorted $procs
  }
}

# The report as lines of text: megabytes (10^6 bytes) per disk, per volume, the -Top processes, and a
# section for the -Names asked about by name (a name not seen writes 0.0, which is an answer too).
function Format-DiskWritesReport {
  param([Parameter(Mandatory)][object]$Measure, [int]$Top = 15, [string[]]$Names = @(), [string]$Title = '')
  $mb = { param($b) '{0,12:N1}' -f ($b / 1e6) }
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("disk-writes: $($Measure.Seconds) s in $($Measure.Samples) samples$(if ($Title) { ", $Title" })")
  $lines.Add('written per physical disk (MB):')
  foreach ($k in $Measure.Disks.Keys) { $lines.Add(('  {0,-28}{1}' -f $k, (& $mb $Measure.Disks[$k]))) }
  $lines.Add('written per volume (MB):')
  foreach ($k in $Measure.Volumes.Keys) { $lines.Add(('  {0,-28}{1}' -f $k, (& $mb $Measure.Volumes[$k]))) }
  $lines.Add("top $Top writing processes, by name (MB asked to write, files, pipes and network; a process shorter than a sample is missed):")
  foreach ($k in @($Measure.Processes.Keys | Select-Object -First $Top)) { $lines.Add(('  {0,-28}{1}' -f $k, (& $mb $Measure.Processes[$k]))) }
  if ($Names.Count -gt 0) {
    $lines.Add('named processes (MB):')
    foreach ($n in $Names) {
      $key = $n.ToLowerInvariant() -replace '\.exe$', ''
      $v = if ($Measure.Processes.Contains($key)) { $Measure.Processes[$key] } else { 0 }
      $lines.Add(('  {0,-28}{1}' -f $key, (& $mb $v)))
    }
  }
  return $lines.ToArray()
}

Export-ModuleMember -Function ConvertFrom-CounterPath, Get-ProcessBaseName, Measure-CounterWrites, Format-DiskWritesReport
