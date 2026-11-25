#requires -Version 7
<#
.SYNOPSIS
  The owner's one elevated run: the frame's GPU metrics split by render-graph pass (roadmap R219).

.DESCRIPTION
  NEVER open the driver's performance counters to all users to run this ("Allow access to the GPU
  performance counters to all users" in the NVIDIA control panel, or the registry key behind it).
  On 2026-11-24 that switch reset the owner's display driver about fifty times and did not stay
  set. GPU metrics on Windows are read by an elevated process instead: run this script ONCE, from
  an administrator PowerShell 7 that the owner opened himself, and hand the output folder back.
  No agent runs it, and no agent asks for the counters (docs/experiments/frame-waits-on-the-card.md).

  Two actions:

  record  Flies engine-view offscreen along a camera path under Nsight Systems with the Vulkan
          trace (each command buffer's GPU workload and the debug-utils labels on the CPU and GPU
          rows) and the GPU metrics sampled on the device (SMs active, SM issue, warps in flight,
          unallocated warps, DRAM read and write, PCIe), engine-view's render graph labelling
          every pass with its name (`renderer.graph.labels=true`, R219) and writing its own frame
          records (`--benchmark`) beside, so the reader can hold the capture's per-pass GPU times
          against the engine's own timer. Elevation is checked first and the script stops without
          it; it takes the machine-wide GPU lock itself (tools/gpu-lock.ps1).

  read    Reads a recorded folder (no elevation, no device): exports the report with Nsight's own
          `nsys export --type jsonlines` (2026.1 and later) and `nsys stats --report
          vulkan_gpu_marker_sum`, then joins every GPU metric sample to the pass label whose GPU
          range holds it, and prints per pass: its share of the labelled GPU time, the median and
          90th percentile of SM issue, warps in flight (as a share of the device's warp slots),
          SMs active, DRAM read and write, and the samples that fell in it. A sample that falls in
          no label (between passes, or in another process's work) is counted as `(none)`.

  What the join rests on, and what is not known until the owner's export is read:
  - GPU metrics are device-wide samples: they know no process, context or pass. The join is by
    time alone, so it is a pass's average over the samples inside its GPU range; a range shorter
    than the sample period (100 us at 10 kHz) gets few or no samples and is reported with its
    count. Passes that overlap on the GPU (the render graph lets passes without a dependency run
    together) give a sample to every range that holds it; `-Serialize` records a full barrier
    before every pass (`renderer.graph.serialize`, R186) so nothing overlaps, at the price of the
    overlap's time — fly once each way when that matters.
  - The GPU rows of Vulkan labels are measured by timestamps the driver inserts, and NVIDIA's own
    forum has reports (2025, and 2026.3.1) of GPU label ranges that overlap or end early around
    ray-tracing work. So the reader also prints, per pass, the capture's GPU time beside the
    engine's own `gpu_ms` from the same flight's frame records: a pass whose two times disagree
    by more than a fifth is marked, and its metrics are not to be trusted.
  - The jsonlines export's table and column names are read from the export itself: the reader
    looks for the metric samples (timestamp, metric id, value), the metric names, the string
    table and the Vulkan debug-label rows with a GPU flag or GPU timestamps. Where one is not
    found it prints every table it did find with its columns and stops, naming what is missing.

.EXAMPLE
  # As administrator, once, the forest staging standing at the owner's settings:
  pwsh tools/gpu-pass-metrics.ps1 record -Exe build/msvc-release/bin/engine-view.exe -Out <dir> `
    -CameraPath <path.json> -Delay 150 -Duration 60 -EngineArgs @('--scene', '<scene.json>', ...)
  pwsh tools/gpu-pass-metrics.ps1 read -Out <dir>
#>
param(
  [Parameter(Position = 0, Mandatory)][ValidateSet('record', 'read')][string]$Action,
  [Parameter(Mandatory)][string]$Out,
  [string]$Exe = '',
  [string]$CameraPath = '',
  [string[]]$EngineArgs = @(),
  [string]$Nsys = '',
  [string]$MetricSet = '',
  [int]$Frequency = 10000,
  [int]$Delay = 150,
  [int]$Duration = 60,
  [switch]$Serialize,
  [switch]$NoLock
)
$ErrorActionPreference = 'Stop'

function Find-Nsys {
  if ($Nsys) { return $Nsys }
  $roots = @("$env:ProgramFiles\NVIDIA Corporation")
  $found = Get-ChildItem $roots -Directory -Filter 'Nsight Systems *' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'target-windows-x64\nsys.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $found) { throw 'nsys.exe not found under Program Files\NVIDIA Corporation; pass -Nsys' }
  return $found
}

function Test-Elevated {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($Action -eq 'record') {
  if (-not (Test-Elevated)) {
    Write-Error ('record needs an administrator PowerShell the owner opened (GPU metrics on Windows). ' +
      'Never open the counters to all users instead: it reset the driver about fifty times.')
    exit 2
  }
  if (-not $Exe -or -not (Test-Path $Exe)) { throw "-Exe: engine-view.exe not found ($Exe)" }
  if (-not $CameraPath -or -not (Test-Path $CameraPath)) { throw "-CameraPath not found ($CameraPath)" }
  $nsys = Find-Nsys
  New-Item -ItemType Directory -Force $Out | Out-Null
  $Out = (Resolve-Path $Out).Path
  $view = @($EngineArgs) + @('--offscreen', '--audio', 'off', '--camera-path', $CameraPath,
    '--tunable', 'renderer.graph.labels=true', '--benchmark', "$Out\frames.jsonl", '--report', "$Out\report.json")
  if ($Serialize) { $view += @('--tunable', 'renderer.graph.serialize=true') }
  $prof = @('profile', '--trace=vulkan', '--vulkan-gpu-workload=individual', '--sample=none', '--cpuctxsw=none',
    '--gpu-metrics-devices=0', "--gpu-metrics-frequency=$Frequency", "--delay=$Delay", "--duration=$Duration",
    '--force-overwrite=true', '-o', "$Out\profile")
  if ($MetricSet) { $prof += "--gpu-metrics-set=$MetricSet" }
  $all = $prof + @($Exe) + $view
  $all | Set-Content "$Out\arguments.txt"
  & $nsys --version | Set-Content "$Out\nsys-version.txt"
  & nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,power.limit --format=csv | Set-Content "$Out\gpu.txt"
  $quoted = ($all | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join ' '
  $cmd = "& '$nsys' $quoted 1> '$Out\stdout.log' 2> '$Out\stderr.log'; exit `$LASTEXITCODE"
  if ($NoLock) { pwsh -NoProfile -Command $cmd }
  else {
    $lock = Join-Path $PSScriptRoot 'gpu-lock.ps1'
    pwsh -NoProfile -File $lock run -Minutes ([math]::Ceiling(($Delay + $Duration) / 60) + 5) `
      -Purpose 'R219: the per-pass GPU metrics, the owner''s elevated Nsight run' -Exec $cmd
  }
  $code = $LASTEXITCODE
  "record: exit $code; hand back $Out (profile.nsys-rep, frames.jsonl, report.json, arguments.txt)"
  exit $code
}

# ---- read ----
$nsys = Find-Nsys
$rep = Join-Path $Out 'profile.nsys-rep'
if (-not (Test-Path $rep)) { throw "no profile.nsys-rep in $Out" }
$jl = Join-Path $Out 'profile.jsonl'
if (-not (Test-Path $jl)) {
  & $nsys export --type jsonlines --force-overwrite=true -o $jl $rep
  if ($LASTEXITCODE) { throw "nsys export --type jsonlines failed ($LASTEXITCODE)" }
}
& $nsys stats --report vulkan_gpu_marker_sum --format csv --force-overwrite=true -o (Join-Path $Out 'stats') $rep | Out-Null

# Pass 1: which tables and columns the export holds. A row names its table by a field the export
# chooses; the first string-valued key among these is taken as it.
$tableKeys = @('table', 'Table', 'type', 'name_of_table')
$tables = @{}
$reader = [IO.StreamReader]::new($jl)
function Get-Table($o) { foreach ($k in $tableKeys) { if ($o.TryGetProperty($k, [ref]$null)) { return $o.GetProperty($k).ToString() } }; return '(untagged)' }
$n = 0
while (($line = $reader.ReadLine()) -ne $null -and $n -lt 2000000) {
  $n++
  $doc = [Text.Json.JsonDocument]::Parse($line); $o = $doc.RootElement
  $t = Get-Table $o
  if (-not $tables.ContainsKey($t)) { $tables[$t] = @{ count = 0; columns = @($o.EnumerateObject() | ForEach-Object Name) } }
  $tables[$t].count++
  $doc.Dispose()
}
$reader.Close()
function Find-Table($pattern, [string[]]$needs) {
  foreach ($k in $tables.Keys) {
    if ($k -match $pattern) {
      $cols = $tables[$k].columns
      if (@($needs | Where-Object { $_ -notin $cols }).Count -eq 0) { return $k }
    }
  }
  return $null
}
$tMetrics = Find-Table 'GPU_METRICS$' @('timestamp', 'metricId', 'value')
$tNames = Find-Table 'TARGET_INFO_GPU_METRICS' @('metricId', 'metricName')
$tStrings = Find-Table 'StringIds' @('id', 'value')
$tLabels = Find-Table 'VULKAN.*(DEBUG|MARKER|LABEL)' @('start', 'end')
$missing = @()
if (-not $tMetrics) { $missing += 'GPU metric samples (a GPU_METRICS table with timestamp, metricId, value)' }
if (-not $tNames) { $missing += 'metric names (TARGET_INFO_GPU_METRICS with metricId, metricName)' }
if (-not $tStrings) { $missing += 'the string table (StringIds with id, value)' }
if (-not $tLabels) { $missing += 'the Vulkan debug labels (a VULKAN_*DEBUG*/MARKER* table with start, end)' }
if ($missing.Count -gt 0) {
  'The export does not hold what the join needs. Missing: ' + ($missing -join '; ')
  'Tables found (rows, columns):'
  foreach ($k in ($tables.Keys | Sort-Object)) { "  {0} ({1}): {2}" -f $k, $tables[$k].count, ($tables[$k].columns -join ', ') }
  exit 3
}
$labelCols = $tables[$tLabels].columns
$textCol = @('textId', 'nameId') | Where-Object { $_ -in $labelCols } | Select-Object -First 1
$gpuCol = @('gpu', 'deviceId') | Where-Object { $_ -in $labelCols } | Select-Object -First 1

# Pass 2: strings, metric names, label ranges (GPU rows only where the table says which rows are
# the GPU's), metric samples.
$strings = @{}; $names = @{}; $ranges = [Collections.Generic.List[object]]::new()
$samples = [Collections.Generic.List[object]]::new()
$reader = [IO.StreamReader]::new($jl)
while (($line = $reader.ReadLine()) -ne $null) {
  $doc = [Text.Json.JsonDocument]::Parse($line); $o = $doc.RootElement; $t = Get-Table $o
  switch ($t) {
    { $_ -eq $tStrings } { $strings[[string]$o.GetProperty('id')] = $o.GetProperty('value').GetString() }
    { $_ -eq $tNames } { $names[[string]$o.GetProperty('metricId')] = $o.GetProperty('metricName').ToString() }
    { $_ -eq $tLabels } {
      $isGpu = $true
      if ($gpuCol) { $g = $o.GetProperty($gpuCol); $isGpu = $g.ValueKind -ne [Text.Json.JsonValueKind]::Null }
      if ($isGpu) {
        $ranges.Add([pscustomobject]@{ start = [int64]$o.GetProperty('start').ToString(); end = [int64]$o.GetProperty('end').ToString()
            text = if ($textCol) { [string]$o.GetProperty($textCol) } else { '' } })
      }
    }
    { $_ -eq $tMetrics } {
      $samples.Add([pscustomobject]@{ t = [int64]$o.GetProperty('timestamp').ToString(); m = [string]$o.GetProperty('metricId'); v = [double]$o.GetProperty('value').ToString() })
    }
  }
  $doc.Dispose()
}
$reader.Close()
if (-not $gpuCol) { "note: the label table names no GPU column; every label row was taken, CPU rows included, which the times below will show" }
foreach ($r in $ranges) { $r.text = if ($strings.ContainsKey($r.text)) { $strings[$r.text] } else { $r.text } }
$ranges = @($ranges | Sort-Object start)
$samples = @($samples | Sort-Object t)

# The join: every sample to every label range holding it (a sweep over both, sorted by time).
$acc = @{}   # pass -> metric -> list of values
$time = @{}  # pass -> summed GPU ns
foreach ($r in $ranges) { $time[$r.text] = $time[$r.text] + ($r.end - $r.start) }
$open = [Collections.Generic.List[object]]::new(); $ri = 0
foreach ($s in $samples) {
  while ($ri -lt $ranges.Count -and $ranges[$ri].start -le $s.t) { $open.Add($ranges[$ri]); $ri++ }
  for ($k = $open.Count - 1; $k -ge 0; $k--) { if ($open[$k].end -lt $s.t) { $open.RemoveAt($k) } }
  $passes = if ($open.Count) { @($open | ForEach-Object text | Select-Object -Unique) } else { @('(none)') }
  foreach ($p in $passes) {
    if (-not $acc.ContainsKey($p)) { $acc[$p] = @{} }
    $mn = $names[$s.m]; if (-not $mn) { $mn = "metric $($s.m)" }
    if (-not $acc[$p].ContainsKey($mn)) { $acc[$p][$mn] = [Collections.Generic.List[double]]::new() }
    $acc[$p][$mn].Add($s.v)
  }
}
function Get-Pct($list, $q) { if (-not $list -or $list.Count -eq 0) { return [double]::NaN }; $a = $list.ToArray(); [Array]::Sort($a); return $a[[int][math]::Floor($q * ($a.Length - 1))] }
function Get-Metric($p, $pattern) { $k = $acc[$p].Keys | Where-Object { $_ -match $pattern } | Select-Object -First 1; if ($k) { return $acc[$p][$k] } }

# The engine's own per-pass times from the same flight, to hold the labels' GPU times against.
$engine = @{}
$frames = Join-Path $Out 'frames.jsonl'
if (Test-Path $frames) {
  $fr = 0
  foreach ($l in [IO.File]::ReadLines($frames)) {
    $j = $l | ConvertFrom-Json -Depth 8; $g = if ($j.gpu_ms) { $j.gpu_ms } elseif ($j.reconstructed) { $j.reconstructed.gpu_ms }
    if (-not $g) { continue }; $fr++
    foreach ($pp in $g.PSObject.Properties) { $engine[$pp.Name] = $engine[$pp.Name] + [double]$pp.Value }
  }
  foreach ($k in @($engine.Keys)) { $engine[$k] = $engine[$k] / [math]::Max(1, $fr) }
}
$total = ($time.Values | Measure-Object -Sum).Sum
'pass | share of labelled GPU time | samples | SM issue p50 / p90 | warps in flight p50 / p90 | unallocated warps p50 | SMs active p50 | DRAM read / write p50'
'---|---:|---:|---:|---:|---:|---:|---:'
foreach ($p in ($acc.Keys | Sort-Object { - $time[$_] })) {
  $issue = Get-Metric $p 'SM Issue'; $warps = Get-Metric $p 'Warps in Flight|Warp Occupancy'
  $unal = Get-Metric $p 'Unallocated Warps'; $act = Get-Metric $p 'SMs? Active'
  $dr = Get-Metric $p 'DRAM Read'; $dw = Get-Metric $p 'DRAM Write'
  $n = ($acc[$p].Values | Select-Object -First 1).Count
  '{0} | {1:P1} | {2} | {3:F1} / {4:F1} | {5:F1} / {6:F1} | {7:F1} | {8:F1} | {9:F1} / {10:F1}' -f $p,
    ($(if ($total) { $time[$p] / $total } else { 0 })), $n, (Get-Pct $issue 0.5), (Get-Pct $issue 0.9),
    (Get-Pct $warps 0.5), (Get-Pct $warps 0.9), (Get-Pct $unal 0.5), (Get-Pct $act 0.5), (Get-Pct $dr 0.5), (Get-Pct $dw 0.5)
}
if ($engine.Count) {
  ''
  'The capture against the engine''s own timer (ms a frame, labelled GPU time over the frames the capture held; the bucket names differ from the pass names where a bucket holds several passes):'
  "engine gpu_ms means: " + (($engine.GetEnumerator() | Where-Object { $_.Value -gt 0.05 } | Sort-Object Value -Descending | ForEach-Object { '{0} {1:F2}' -f $_.Key, $_.Value }) -join ', ')
}
