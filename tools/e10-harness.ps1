#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Experiment E10's harness: take a folder of generated GLBs through the content build and the
  renderer, and say which of them the engine can use as they are and what the rest would need.

.DESCRIPTION
  tools/e10-harness.ps1 -Folder <dir of .glb>... [-Out <dir>] [-Bin <dir> | -Preset msvc-release]
                        [-Width 640] [-Height 640] [-Orbit 16] [-Frames 4] [-CoarseLod 1] [-FinestLod 0.05]
                        [-Shadows off|rt] [-MaxFlip 0.02] [-MinIslandTexels 1] [-MaxWarnings 0]
                        [-Only <name,...>] [-KeepContainers] [-Title <text>]

  For every GLB (any service; a provenance sidecar beside it, from tools/generate.ps1, adds the
  service, the task and the credits to its row):

    1. engine-content build   -> triangles, clusters, levels, warnings (by rule), container bytes,
                                 build ms; an import the build refuses is a failure with its rule
    2. engine-content stats   -> the atlas: islands, seam fraction, smallest island in texels of a
                                 4096 atlas; and the CPU attribute-error table for context
    3. engine-view, twice     -> the same frame at the default LOD threshold (-CoarseLod, 1 px) and
                                 at -FinestLod (0.05 px, effectively the leaves), from a fixed
                                 framing orbit, offscreen-sized captures
    4. engine-image compare   -> PSNR, SSIM and FLIP of coarse against finest, and a FLIP heat map

  and writes <Out>/report.json and <Out>/report.md: one row per asset, a pass or a fail against the
  thresholds below with a one-line diagnosis naming what a repair step would have to do, and the
  totals (pass rate, mean and worst of every column).

  THE THRESHOLDS, and why those.
    -MaxWarnings 0        an asset that needs a human to read a warning before it is used is not
                          an asset the pipeline can take unattended, which is the question E10 asks.
    -MinIslandTexels 1    an atlas island smaller than one texel of a 4096 atlas cannot be sampled
                          as itself at any mip, and no seam-respecting simplification can make it
                          coarser (docs/subsystems/geometry.md, "What the simplifier is given"): the
                          only repair is in texture space. The two Meshy characters that motivated
                          the atlas statistics both had an island that rounds to zero.
    -MaxFlip 0.02         the seam fix's own numbers (geometry.md): the defective builds of those two
                          characters measured 0.0241 and 0.0223 coarse-against-finest, the fixed ones
                          0.0115 and 0.0083, and the FlightHelmet 0.0173 — so 0.02 passes every
                          known-good asset and fails every known-bad one. It is a whole-image mean at
                          a fixed framing, like those were; the report also gives the object's share
                          of the frame and the mean over the object alone, so a thin prop that
                          covers little of the frame can be read fairly.

  WHICH BINARIES. Never the ones in a build tree in place: the harness copies engine-content,
  engine-view and engine-image (from -Bin, or from build/<Preset>/bin of this checkout) into
  <Out>/bin and runs the copies, so a rebuild of that tree during a run neither fails nor changes
  the run, and the report records each copy's SHA-256 and the commit it was built from.

  THE GPU. A capture is a few frames of a small scene; forty of them do not need the machine-wide
  GPU lock (docs/subsystems/bench.md), and the picture metrics do not depend on load. The build
  milliseconds do, so the report carries the machine state engine-view recorded around every
  capture and the lock's holder at the start and the end, and a write-up quotes them.

.EXAMPLE
  tools/e10-harness.ps1 -Folder D:\workspace\game_engine_local\generated\meshy\2026-09-22
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string[]]$Folder,
  [string]$Out,
  [string]$Bin,
  [string]$Preset = 'msvc-release',
  [int]$Width = 640,
  [int]$Height = 640,
  [double]$Orbit = 16,
  [int]$Frames = 4,
  [double]$CoarseLod = 1.0,
  [double]$FinestLod = 0.05,
  [ValidateSet('off', 'rt')] [string]$Shadows = 'off',
  [double]$MaxFlip = 0.02,
  [double]$MinIslandTexels = 1.0,
  [int]$MaxWarnings = 0,
  [string[]]$Only,
  [switch]$KeepContainers,
  [string]$Title,
  [string]$LocalRoot
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$Exe = if ($IsWin) { '.exe' } else { '' }
$Inv = [Globalization.CultureInfo]::InvariantCulture

function Write-Log([string]$text) { [Console]::Error.WriteLine($text) }
function Get-FileSha256([string]$file) { (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() }
function Write-JsonFile([string]$file, $value) {
  [IO.File]::WriteAllText($file, ($value | ConvertTo-Json -Depth 30) + "`n", (New-Object Text.UTF8Encoding($false)))
}
function Get-LastJsonLine($lines) {
  $last = @($lines | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') }) | Select-Object -Last 1
  if (-not $last) { return $null }
  return ($last | ConvertFrom-Json)
}
function Num($v) { if ($null -eq $v) { return $null }; return [double]$v }

# ---- where things go -------------------------------------------------------------------------------

$folders = @()
foreach ($f in $Folder) { foreach ($p in ($f -split ',')) { if ($p.Trim()) { $folders += (Resolve-Path -LiteralPath $p.Trim()).Path } } }
$glbs = @()
foreach ($f in $folders) { $glbs += @(Get-ChildItem -LiteralPath $f -File -Filter '*.glb' | Sort-Object Name) }
if ($Only) {
  $wanted = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
  $glbs = @($glbs | Where-Object { $wanted -contains $_.BaseName })
}
if ($glbs.Count -eq 0) { throw "no .glb files in $($folders -join ', ')" }

if (-not $Out) {
  $root = if ($LocalRoot) { $LocalRoot } elseif ($env:ENGINE_LOCAL_ROOT) { $env:ENGINE_LOCAL_ROOT } elseif ($IsWin) { 'D:\workspace\game_engine_local' } else { Join-Path $HOME 'game_engine_local' }
  $leaf = Split-Path -Leaf $folders[0]; $parent = Split-Path -Leaf (Split-Path -Parent $folders[0])
  $Out = Join-Path (Join-Path $root 'e10') "$parent-$leaf"
}
$Out = [IO.Path]::GetFullPath($Out)
$repoPrefix = $RepoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($Out.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "-Out '$Out' is inside the repository; captures and containers of generated assets are never committed" }
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'bin'), (Join-Path $Out 'assets') | Out-Null

# ---- the binaries: copies, never the build tree in place ------------------------------------------

$source = if ($Bin) { (Resolve-Path -LiteralPath $Bin).Path } else { Join-Path $RepoRoot "build/$Preset/bin" }
$tools = @('engine-content', 'engine-view', 'engine-image')
$binaries = @()
foreach ($t in $tools) {
  $src = Join-Path $source "$t$Exe"
  if (-not (Test-Path -LiteralPath $src)) { throw "no $t$Exe in $source. Build it (tools/dev.ps1 build -Preset $Preset) or pass -Bin <dir>." }
  Copy-Item -LiteralPath $src -Destination (Join-Path $Out 'bin') -Force
  $binaries += [ordered]@{ name = "$t$Exe"; sha256 = Get-FileSha256 (Join-Path (Join-Path $Out 'bin') "$t$Exe"); built = (Get-Item -LiteralPath $src).LastWriteTimeUtc.ToString('o') }
}
Get-ChildItem -LiteralPath $source -File -Filter '*.dll' -ErrorAction SilentlyContinue | Copy-Item -Destination (Join-Path $Out 'bin') -Force
$content = Join-Path (Join-Path $Out 'bin') "engine-content$Exe"
$view = Join-Path (Join-Path $Out 'bin') "engine-view$Exe"
$image = Join-Path (Join-Path $Out 'bin') "engine-image$Exe"
$commit = ''
try { $commit = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim() } catch { }

function Get-LockStatus {
  try { return ((& (Join-Path $PSScriptRoot 'gpu-lock.ps1') status *>&1 | Out-String).Trim()) } catch { return 'unknown' }
}

# The object's share of the frame: engine-view clears to one flat sky colour, so every pixel that is
# not the corner pixel's colour is the object. Windows only (System.Drawing); null elsewhere.
function Get-Coverage([string]$png) {
  if (-not $IsWin) { return $null }
  try {
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
      $rect = [System.Drawing.Rectangle]::new(0, 0, $bmp.Width, $bmp.Height)
      $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
      $n = $data.Stride * $bmp.Height
      $buf = New-Object byte[] $n
      [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $n)
      $bmp.UnlockBits($data)
    } finally { $bmp.Dispose() }
    $px = New-Object uint32[] ($n / 4)
    [Buffer]::BlockCopy($buf, 0, $px, 0, $n)
    $bg = $px[0]; $other = 0
    foreach ($v in $px) { if ($v -ne $bg) { $other++ } }
    return [math]::Round($other / $px.Length, 4)
  } catch { return $null }
}

# ---- the repair each failure asks for -------------------------------------------------------------

$RuleRepairs = @{
  'geometry.degenerate_triangle' = 'drop zero-area triangles (weld, then remove degenerates) before import'
  'geometry.nan_position'        = 'drop or reject non-finite vertices; the source is corrupt'
  'geometry.index_range'         = 'the index buffer points past the vertices; re-export'
  'geometry.empty_primitive'     = 'remove the empty primitive; re-export'
  'geometry.cluster_budget'      = 'decimate before import; more clusters than a visibility id can name'
  'geometry.uv_range'            = 'renormalize the UV set into a few wraps (a UV pass; the texture is untouched)'
  'material.missing_image'       = 're-embed the missing texture or clear the slot'
}

function Get-Diagnosis($row) {
  if ($row.checks.import -eq 'fail') {
    $fix = if ($RuleRepairs.ContainsKey($row.build_rule)) { $RuleRepairs[$row.build_rule] } else { 'see the build error' }
    return "import refused ($($row.build_rule)): $fix"
  }
  $parts = @()
  if ($row.checks.warnings -eq 'fail') {
    $rules = @($row.warning_rules.PSObject.Properties | ForEach-Object { "$($_.Name) x$($_.Value)" }) -join ', '
    $fixes = @($row.warning_rules.PSObject.Properties.Name | ForEach-Object { if ($RuleRepairs.ContainsKey($_)) { $RuleRepairs[$_] } }) -join '; '
    $parts += "$($row.warnings) warnings ($rules): $fixes"
  }
  if ($row.checks.island -eq 'fail') {
    $parts += ("smallest of {0} atlas islands is {1:0.###} texels of 4096 over {2} triangles: repack the UVs so no island is below a texel (merge or enlarge the slivers) and rebake the textures onto the new layout; the geometry builder cannot help" -f $row.islands, $row.smallest_island_texels_4096, $row.smallest_island_triangles)
  }
  if ($row.checks.flip -eq 'fail') {
    $seam = [math]::Round(100 * $row.seam_fraction, 1)
    $parts += ("coarse cut is FLIP {0:0.0000} / PSNR {1:0.0} dB from the finest, with {2} islands and {3}% of vertices on a seam: the coarse levels keep the atlas's seams and still smear across them, so the fix is in texture space (repack into fewer, larger islands, or bake per-level textures); check the heat map for thin parts collapsing instead" -f $row.flip_mean, $row.psnr, $row.islands, $seam)
  }
  if ($row.checks.flip -eq 'unmeasured') { $parts += 'no picture: engine-view could not render here (exit 3)' }
  return ($parts -join ' | ')
}

# ---- the run --------------------------------------------------------------------------------------

$started = [DateTime]::UtcNow
$lockAtStart = Get-LockStatus
Write-Log "e10: $($glbs.Count) GLBs from $($folders -join ', ')"
Write-Log "e10: binaries copied from $source (commit $commit) into $(Join-Path $Out 'bin')"
Write-Log "e10: GPU lock at start: $lockAtStart (captures do not take it)"

$rows = @()
$states = @()
$warned = 0
$index = 0
foreach ($glb in $glbs) {
  $index++
  $name = $glb.BaseName
  $dir = Join-Path (Join-Path $Out 'assets') $name
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  Write-Log ("[{0}/{1}] {2} ({3:N1} MB)" -f $index, $glbs.Count, $name, ($glb.Length / 1MB))

  $row = [ordered]@{ name = $name; source = $glb.FullName; glb_bytes = $glb.Length; glb_sha256 = Get-FileSha256 $glb.FullName }
  $sidecar = Join-Path $glb.DirectoryName "$name.provenance.json"
  if (Test-Path -LiteralPath $sidecar) {
    $s = Get-Content -Raw -LiteralPath $sidecar | ConvertFrom-Json
    $row.service = [string]$s.service
    $row.credits = $s.credits
    $row.generator = [string]$s.asset_provenance.generator
    $row.model_version = [string]$s.asset_provenance.model_version
    if ($s.backend.meshy) { $row.task_id = [string]$s.backend.meshy.task_id }
    if ($s.prompt) { $row.subject = [string]$s.prompt.subject }
  } else {
    $row.service = Split-Path -Leaf (Split-Path -Parent $glb.DirectoryName)
  }
  $row.checks = [ordered]@{ import = 'pass'; warnings = 'pass'; island = 'pass'; flip = 'pass' }

  # 1. build
  $clusters = Join-Path $dir "$name.clusters"
  $buildOut = & $content build $glb.FullName $clusters 2> (Join-Path $dir 'build.err')
  $buildCode = $LASTEXITCODE
  $buildErr = "$(Get-Content -Raw -LiteralPath (Join-Path $dir 'build.err') -ErrorAction SilentlyContinue)"
  $b = Get-LastJsonLine $buildOut
  $rules = [ordered]@{}
  foreach ($m in [regex]::Matches($buildErr, '\b((?:geometry|material)\.[a-z_]+)\b')) {
    $k = $m.Groups[1].Value; if ($rules.Contains($k)) { $rules[$k]++ } else { $rules[$k] = 1 }
  }
  if ($buildCode -ne 0 -or -not $b) {
    $row.checks.import = 'fail'; $row.checks.warnings = 'n/a'; $row.checks.island = 'n/a'; $row.checks.flip = 'n/a'
    $first = @($rules.Keys)[0]
    $row.build_rule = if ($first) { $first } else { "exit $buildCode" }
    $row.build_error = ([string]$buildErr).Trim().Split("`n")[0]
    $row.status = 'fail'
    $row.diagnosis = Get-Diagnosis ([pscustomobject]$row)
    $rows += [pscustomobject]$row
    Write-Log "  import refused: $($row.build_error)"
    continue
  }
  $row.triangles = [long]$b.triangles; $row.clusters = [long]$b.clusters; $row.lod_levels = [int]$b.lod_levels
  $row.materials = [int]$b.materials; $row.images = [int]$b.images
  $row.warnings = [int]$b.warnings; $row.warning_rules = [pscustomobject]$rules
  $row.container_bytes = [long]$b.bytes; $row.build_ms = [math]::Round([double]$b.build_ms, 1)
  if ($row.warnings -gt $MaxWarnings) { $row.checks.warnings = 'fail' }

  # 2. stats
  $statsOut = & $content stats $clusters 2> (Join-Path $dir 'stats.err')
  $st = Get-LastJsonLine $statsOut
  if ($st -and $st.atlas) {
    $a = $st.atlas
    $row.islands = [long]$a.islands; $row.seam_fraction = [math]::Round([double]$a.seam_fraction, 4)
    $row.smallest_island_texels_4096 = [math]::Round([double]$a.smallest_island_texels_4096, 4)
    $row.smallest_island_triangles = [long]$a.smallest_island_triangles
    $row.median_island_texels_4096 = [math]::Round([double]$a.median_island_texels_4096, 1)
    $row.uv_area = [math]::Round([double]$a.uv_area, 4)
    if ($row.smallest_island_texels_4096 -lt $MinIslandTexels) { $row.checks.island = 'fail' }
    $row.lod_attribute_error = $st.lod_attribute_error
  } else { $row.checks.island = 'unmeasured' }

  # 3. two captures, one frame apart only in the LOD threshold
  $common = @('--mesh', $clusters, '--width', $Width, '--height', $Height, '--frames', $Frames, '--orbit', $Orbit.ToString($Inv), '--no-vsync', '--shadows', $Shadows)
  $picture = $true
  foreach ($cut in @(@{ tag = 'coarse'; lod = $CoarseLod }, @{ tag = 'finest'; lod = $FinestLod })) {
    $png = Join-Path $dir "$($cut.tag).png"
    $vOut = & $view @common --lod ($cut.lod.ToString($Inv)) --capture $png 2> (Join-Path $dir "view-$($cut.tag).err")
    $code = $LASTEXITCODE
    $v = Get-LastJsonLine $vOut
    if ($code -eq 3) { $picture = $false; break }
    if ($code -ne 0 -or -not $v -or -not $v.captured) { throw "engine-view failed on $name ($($cut.tag), exit $code): $(Get-Content -Raw (Join-Path $dir "view-$($cut.tag).err"))" }
    $row["$($cut.tag)_visible_pairs"] = [long]$v.visible_pairs_last
    if ($v.machine_state) { $states += $v.machine_state }
    if ((Get-Content -Raw (Join-Path $dir "view-$($cut.tag).err")) -match 'WARNING:') { $warned++ }
  }
  if (-not $picture) {
    $row.checks.flip = 'unmeasured'
  } else {
    $cmp = & $image compare --json (Join-Path $dir 'coarse.png') (Join-Path $dir 'finest.png') --flip (Join-Path $dir 'flip.png') 2> (Join-Path $dir 'compare.err') | ConvertFrom-Json
    $row.psnr = if ($null -eq $cmp.psnr) { $null } else { [math]::Round([double]$cmp.psnr, 2) }
    $row.ssim = [math]::Round([double]$cmp.ssim, 5)
    $row.flip_mean = [math]::Round([double]$cmp.flip_mean, 5)
    $row.flip_p95 = [math]::Round([double]$cmp.flip_p95, 5)
    $row.flip_max = [math]::Round([double]$cmp.flip_max, 4)
    $row.coverage = Get-Coverage (Join-Path $dir 'finest.png')
    $row.flip_object_mean = if ($row.coverage -gt 0) { [math]::Round($row.flip_mean / $row.coverage, 5) } else { $null }
    if ($row.flip_mean -gt $MaxFlip) { $row.checks.flip = 'fail' }
  }

  $failedChecks = @($row.checks.Keys | Where-Object { $row.checks[$_] -in 'fail', 'unmeasured' })
  $row.status = if ($failedChecks.Count -eq 0) { 'pass' } else { 'fail' }
  $row.diagnosis = if ($failedChecks.Count -eq 0) { '' } else { Get-Diagnosis ([pscustomobject]$row) }
  if (-not $KeepContainers) { Remove-Item -LiteralPath $clusters -Force -ErrorAction SilentlyContinue }
  Write-Log ("  {0}: {1:N0} tri, {2} clusters, {3} levels, {4} warn, {5} islands, seam {6:P1}, smallest {7:0.###} texels, FLIP {8}, PSNR {9}" -f `
      $row.status, $row.triangles, $row.clusters, $row.lod_levels, $row.warnings, $row.islands, $row.seam_fraction, $row.smallest_island_texels_4096, $row.flip_mean, $row.psnr)
  $rows += [pscustomobject]$row
}
$finished = [DateTime]::UtcNow
$lockAtEnd = Get-LockStatus

# ---- totals ----------------------------------------------------------------------------------------

# Worst means the direction that hurts: most triangles, most warnings, smallest island, lowest PSNR.
$columns = [ordered]@{
  triangles = 'max'; clusters = 'max'; lod_levels = 'min'; warnings = 'max'; islands = 'max'; seam_fraction = 'max'
  smallest_island_texels_4096 = 'min'; container_bytes = 'max'; build_ms = 'max'; psnr = 'min'; flip_mean = 'max'
  coverage = 'min'; flip_object_mean = 'max'
}
$colStats = [ordered]@{}
foreach ($c in $columns.Keys) {
  $vals = @($rows | Where-Object { $_.PSObject.Properties[$c] -and $null -ne $_.$c } | ForEach-Object { [pscustomobject]@{ name = $_.name; v = [double]$_.$c } })
  if ($vals.Count -eq 0) { continue }
  $mean = ($vals | Measure-Object v -Average).Average
  $worst = if ($columns[$c] -eq 'max') { $vals | Sort-Object v -Descending | Select-Object -First 1 } else { $vals | Sort-Object v | Select-Object -First 1 }
  $best = if ($columns[$c] -eq 'max') { $vals | Sort-Object v | Select-Object -First 1 } else { $vals | Sort-Object v -Descending | Select-Object -First 1 }
  $colStats[$c] = [ordered]@{ n = $vals.Count; mean = [math]::Round($mean, 5); worst = $worst.v; worst_asset = $worst.name; best = $best.v; best_asset = $best.name }
}
$passed = @($rows | Where-Object status -eq 'pass').Count
$byCheck = [ordered]@{}
foreach ($k in @('import', 'warnings', 'island', 'flip')) { $byCheck[$k] = @($rows | Where-Object { $_.checks[$k] -in 'fail', 'unmeasured' }).Count }

function Get-StateSpan([string]$field) {
  $v = @($states | ForEach-Object { $_.start.$field; $_.end.$field } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
  if ($v.Count -eq 0) { return $null }
  return [ordered]@{ min = [math]::Round(($v | Measure-Object -Minimum).Minimum, 1); max = [math]::Round(($v | Measure-Object -Maximum).Maximum, 1) }
}
$machine = [ordered]@{
  samples = $states.Count * 2
  cpu_others_pct = Get-StateSpan 'cpu_others_pct'
  gpu_util_pct = Get-StateSpan 'gpu_util_pct'
  gpu_memory_used_mib = Get-StateSpan 'gpu_memory_used_mib'
  gpu_memory_total_mib = Get-StateSpan 'gpu_memory_total_mib'
  captures_warned = $warned
  gpu_lock_at_start = $lockAtStart
  gpu_lock_at_end = $lockAtEnd
}

$report = [ordered]@{
  schema = 'engine.e10.report/1'
  title = $(if ($Title) { $Title } else { "E10 over $((Split-Path -Leaf (Split-Path -Parent $folders[0])))/$(Split-Path -Leaf $folders[0])" })
  started_utc = $started.ToString('o'); finished_utc = $finished.ToString('o')
  seconds = [math]::Round(($finished - $started).TotalSeconds, 1)
  folders = $folders
  tools = [ordered]@{ source = $source; commit = $commit; preset = $(if ($Bin) { $null } else { $Preset }); binaries = $binaries; harness = 'tools/e10-harness.ps1' }
  settings = [ordered]@{ width = $Width; height = $Height; orbit = $Orbit; frames = $Frames; coarse_lod_px = $CoarseLod; finest_lod_px = $FinestLod; shadows = $Shadows; raster = 'hw (default)' }
  thresholds = [ordered]@{
    max_warnings = $MaxWarnings; min_island_texels_4096 = $MinIslandTexels; max_flip_mean = $MaxFlip
    why = [ordered]@{
      warnings = 'an asset that needs a person to read a warning before use is not one the pipeline can take unattended'
      island = 'an island under one texel of a 4096 atlas cannot be sampled as itself and no seam-respecting simplification can coarsen it; the repair is in texture space'
      flip = 'geometry.md seam-fix numbers: defective builds 0.0241 and 0.0223, fixed builds 0.0115 and 0.0083, FlightHelmet 0.0173; 0.02 separates them'
    }
  }
  machine_state = $machine
  totals = [ordered]@{ assets = $rows.Count; passed = $passed; pass_rate = $(if ($rows.Count) { [math]::Round($passed / $rows.Count, 3) } else { 0 }); failed_by_check = $byCheck; columns = $colStats }
  assets = $rows
}
Write-JsonFile (Join-Path $Out 'report.json') $report

# ---- report.md -------------------------------------------------------------------------------------

function F($v, [string]$fmt) { if ($null -eq $v -or ($v -is [string] -and $v -eq '')) { return '—' }; return ([double]$v).ToString($fmt, $Inv) }
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# $($report.title)")
$md.Add('')
$md.Add("Generated by ``tools/e10-harness.ps1`` on $($finished.ToString('yyyy-MM-dd HH:mm')) UTC in $($report.seconds) s, binaries from commit ``$($commit.Substring(0, [math]::Min(12, $commit.Length)))`` ($source, copied).")
$md.Add("Captures: ${Width}x${Height}, ``--orbit $Orbit --frames $Frames --shadows $Shadows``, coarse ``--lod $CoarseLod`` against finest ``--lod $FinestLod``.")
$md.Add('')
$md.Add("**Pass rate: $passed of $($rows.Count) ($([math]::Round(100 * $report.totals.pass_rate))%).** Failures by check: import $($byCheck.import), warnings $($byCheck.warnings), atlas island $($byCheck.island), coarse-vs-finest FLIP $($byCheck.flip) (an asset can fail more than one).")
$md.Add('')
$md.Add("Thresholds: 0 warnings; smallest atlas island >= $MinIslandTexels texel of a 4096 atlas; coarse-vs-finest FLIP mean <= $MaxFlip (the seam fix's numbers put every known-good build below it and every known-bad one above).")
$md.Add('')
$md.Add('| asset | triangles | clusters | levels | warn | islands | seam % | smallest island (texels) | container MB | build ms | PSNR dB | FLIP | object FLIP | result |')
$md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
foreach ($r in $rows) {
  $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} |' -f $r.name, (F $r.triangles 'N0'), (F $r.clusters 'N0'), (F $r.lod_levels '0'),
      (F $r.warnings '0'), (F $r.islands 'N0'), (F $(if ($null -ne $r.seam_fraction) { 100 * $r.seam_fraction }) '0.0'), (F $r.smallest_island_texels_4096 '0.###'),
      (F $(if ($r.container_bytes) { $r.container_bytes / 1MB }) '0.0'), (F $r.build_ms 'N0'), (F $r.psnr '0.0'), (F $r.flip_mean '0.0000'), (F $r.flip_object_mean '0.000'),
      $(if ($r.status -eq 'pass') { 'pass' } else { '**fail**' })))
}
$md.Add('')
$md.Add('| column | mean | worst | worst asset | best | best asset |')
$md.Add('|---|---|---|---|---|---|')
foreach ($c in $colStats.Keys) { $s = $colStats[$c]; $md.Add("| $c | $(F $s.mean 'G5') | $(F $s.worst 'G5') | $($s.worst_asset) | $(F $s.best 'G5') | $($s.best_asset) |") }
$md.Add('')
$fails = @($rows | Where-Object status -ne 'pass')
if ($fails.Count -gt 0) {
  $md.Add('## Failures, and what a repair step would have to do')
  $md.Add('')
  foreach ($r in $fails) { $md.Add("- **$($r.name)** — $($r.diagnosis)") }
  $md.Add('')
}
$md.Add('## Machine state')
$md.Add('')
$ms = $machine
$md.Add(("Sampled by engine-view before and after each of its {0} runs: other processes at {1}–{2}% of the CPU, the GPU {3}–{4}% busy with {5}–{6} MiB of {7} in use; {8} capture runs printed the busy-machine WARNING. GPU lock at the start: {9}; at the end: {10}. The picture metrics do not depend on load; the build milliseconds are upper bounds whenever the CPU figure is high." -f `
    ($states.Count), $ms.cpu_others_pct.min, $ms.cpu_others_pct.max, $ms.gpu_util_pct.min, $ms.gpu_util_pct.max, $ms.gpu_memory_used_mib.min, $ms.gpu_memory_used_mib.max, $ms.gpu_memory_total_mib.max, $warned, $lockAtStart, $lockAtEnd))
[IO.File]::WriteAllText((Join-Path $Out 'report.md'), (($md -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))

Write-Log "e10: $passed of $($rows.Count) pass; report at $(Join-Path $Out 'report.md')"
[Console]::Out.WriteLine(([ordered]@{ report = (Join-Path $Out 'report.json'); markdown = (Join-Path $Out 'report.md'); assets = $rows.Count; passed = $passed; pass_rate = $report.totals.pass_rate } | ConvertTo-Json -Compress))
