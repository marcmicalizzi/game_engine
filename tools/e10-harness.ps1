#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Experiment E10's harness: take a folder of generated GLBs through the content build and the
  renderer, and say which of them the engine can use as they are and what the rest would need.

.DESCRIPTION
  tools/e10-harness.ps1 -Folder <dir of .glb>... [-Out <dir>] [-Bin <dir> | -Preset msvc-release]
                        [-Width 640] [-Height 640] [-Orbit 0] [-Frames 4] [-CoarseLod 1] [-FinestLod 0.05]
                        [-Shadows off|rt] [-Offscreen] [-Only <name,...>] [-KeepContainers] [-Title <text>]
                        [-MaxFlip 0.02] [-MinIslandTexels 1] [-MaxWarnings 0] [-DenseTriangles 250000]
                        [-LowCoverage 0.15] [-MaxCollapseShare 0.75] [-MinCollapseDensity 3]
                        [-AllowStale] [-StalenessPaths <path,...>]
  tools/e10-harness.ps1 -FromReport <report.json> [the threshold flags]
  tools/e10-harness.ps1 -CheckBinaries -Out <scratch dir> [-Bin <dir> | -Preset msvc-release]   # the staleness check alone

  For every GLB, or .gltf beside its buffers (any service; a provenance sidecar beside it, from
  tools/generate.ps1, adds the service, the task and the credits to its row):

    1. engine-content build   -> triangles, clusters, levels, warnings (by rule), container bytes,
                                 build ms; an import the build refuses is a failure with its rule
    2. engine-content stats   -> the atlas: islands, seam fraction, smallest island in texels of a
                                 4096 atlas; and the CPU attribute-error table for context
    3. engine-view, twice     -> the same frame at the default LOD threshold (-CoarseLod, 1 px) and
                                 at -FinestLod (0.05 px, effectively the leaves), from an orbit that
                                 frames the whole bounds (below)
    4. engine-image compare   -> PSNR, SSIM and FLIP of coarse against finest, and a FLIP heat map

  and writes <Out>/report.json and <Out>/report.md: one row per asset, a pass or a fail against the
  thresholds below with a one-line diagnosis naming what a repair step would have to do, the totals
  (pass rate, mean, worst and best of every column), and what the picture error correlates with.

  Measuring and judging are separate steps: `-FromReport` re-reads a report's measurements and
  judges them again under whatever thresholds are given, rewriting report.json and report.md in
  place without building or rendering anything — a threshold is a decision, and changing one
  should not cost forty captures.

  THE THRESHOLDS, and why those.
    -MaxWarnings 0        an asset that needs a person to read a warning before it is used is not
                          an asset the pipeline can take unattended, which is the question E10 asks.
    -MinIslandTexels 1    an atlas island smaller than one texel of a 4096 atlas cannot be sampled
                          as itself at any mip, and no seam-respecting simplification can make it
                          coarser (docs/subsystems/geometry.md, "What the simplifier is given"). The
                          two Meshy characters that motivated the atlas statistics both had an island
                          that rounds to zero.
    -MaxFlip 0.02         the seam fix's own numbers (geometry.md): the defective builds of those two
                          characters measured 0.0241 and 0.0223 coarse-against-finest, the fixed ones
                          0.0115 and 0.0083, and the FlightHelmet 0.0173 — so 0.02 passes every
                          known-good asset and fails every known-bad one. It is a whole-image mean at
                          a fixed framing, like those were; the report also gives the object's share
                          of the frame and the mean over the object alone, because the whole-frame
                          mean follows how much of the frame the object covers (E10's first pass:
                          r = 0.70, against 0.02 for the object-only mean).
    -DenseTriangles       not a pass criterion: the size above which a FLIP failure is diagnosed as
                          detail carried by geometry rather than by the atlas (E10's first pass: the
                          picture error followed log triangle count at r = 0.81).
    -MaxCollapseShare 0.75, judged where the finest cut is at least -MinCollapseDensity 3 pairs per
                          1,000 pixels of the object: how far the LOD collapses. The coarse cut (1 px)
                          may draw at most three quarters of the finest cut's visible (instance,
                          cluster) pairs. A mesh whose LOD does not collapse passes the FLIP check for
                          the wrong reason — a cut that did not coarsen has nothing to lose — while
                          it costs its finest cut at every distance: E10's second pass (Meshy's
                          triangle remesh) had six such, their coarse cut 86-100% of the finest, the
                          share following the seam fraction at r = 0.94. The line is from evidence,
                          measured at the E10 sets' framing (640x640, orbit 16, 1 px against 0.05 px)
                          and at the fit framing: the known-good Khronos samples the check judges draw
                          at most 46% (FlightHelmet); at orbit 16 every mesh not made by the triangle
                          remesh draws at most 68% (Tripo's barrel cactus), and the six that do not
                          coarsen at least 86% — 0.75 is between, and means the LOD must shed at least
                          a quarter of the cut. (Framed whole, the six draw 74-100%, and Tripo's rope
                          fails at 84%: a sound atlas, but strands at the scale of the view.) The
                          share depends on the view, so it is judged only where the finest cut is
                          denser on screen than the threshold can resolve: a low-poly game asset whose
                          triangles are already several pixels each (SciFiHelmet: 23,000 triangles,
                          share 0.96 at orbit 16 and 0.85 framed whole, at 1.3 and 2.3 pairs per
                          1,000 object pixels) has nothing to shed at this view and is not at fault,
                          while every mesh that did not coarsen was at 4.0 or more; 3 is between.
                          Off Windows there is no coverage, so no density, and every mesh is judged.

    -LowCoverage 0.15     not a pass criterion: below this share of the frame the object-only FLIP is
                          flagged as unreliable in the report. It is the whole-frame mean divided by
                          the coverage, so on a thin object — nearly every pixel of it a silhouette
                          pixel, where a coarse cut's sub-pixel edge shifts score high, and some of
                          the error falling outside the finest cut's mask altogether — it inflates:
                          a bare tree at 10% coverage scored 0.108 over the object while its whole
                          frame passed at 0.0109. Read its heat map instead.

  THE FRAMING. engine-view's orbit (systems/renderer view_set.cpp) aims at the centre of the
  mesh's bounds from `orbit` tenths of the bounding radius out, 0.45 of that above, with a 55°
  vertical field of view. -Orbit 0 (the default) picks the nearest orbit at which the whole
  bounding sphere is in frame with a 5% margin — 20.8 for a square frame — so a tall trunk or a
  long log is framed whole from any side. The first E10 pass used a fixed 16, which puts the
  camera at 1.75 radii where the sphere needs 2.17, and cut a tall tree's trunk in half; its
  numbers are not comparable with this framing, and -Orbit 16 reproduces them.

  WHICH BINARIES. Never the ones in a build tree in place: the harness copies engine-content,
  engine-view and engine-image (from -Bin, or from build/<Preset>/bin of this checkout) into
  <Out>/bin and runs the copies, so a rebuild of that tree during a run neither fails nor changes
  the run, and the report records each copy's SHA-256 and the commit it was built from.

  AND NEVER STALE ONES. On 2026-09-22 a pass measured with build/msvc-release binaries hours older
  than the HEAD it recorded, which silently dropped every atlas statistic and every embedded
  texture. So before anything is measured each copy is asked for its build stamp (`--version`, the
  commit the tree was at when it was built; cmake/EngineBuildStamp.cmake) and is stale when any
  file under -StalenessPaths changed between that commit and HEAD. A binary with no stamp (built
  before stamps existed) is judged by time instead: stale when it is older than the last commit
  touching those paths — which cannot see a commit fast-forwarded in with an older date, which is
  why the stamp comes first. Either way, a working-tree edit under those paths newer than the
  binary makes it stale too. A stale binary is refused with the rebuild command, unless
  -AllowStale, and then report.md and report.json open with a STALE BINARIES caveat.

  THE GPU. A capture is a few frames of a small scene; forty of them do not need the machine-wide
  GPU lock (docs/subsystems/bench.md), and the picture metrics do not depend on load. The build
  milliseconds do, so the report carries the machine state engine-view recorded around every
  capture and the lock's holder at the start and the end, and a write-up quotes them.
  -Offscreen draws both cuts without a window, so none opens on a machine someone is using: through
  engine-host --stdio (`render.load`, `render.capture`, which draws offscreen), from the camera
  engine-view's orbit gives the same frame, reading the visible pairs from the capture's stats.
  `engine-view --offscreen` cannot do it: its plain offscreen summary has no visible-pair count,
  which the collapse check is judged on. engine-host samples no machine state, so an offscreen
  report says so instead of quoting one; the report records the mode.

.EXAMPLE
  tools/e10-harness.ps1 -Folder D:\workspace\game_engine_local\generated\meshy\2026-09-22
  tools/e10-harness.ps1 -FromReport D:\workspace\game_engine_local\e10\meshy-2026-09-22\report.json -MaxFlip 0.03
#>
[CmdletBinding()]
param(
  [string[]]$Folder,
  [string]$FromReport,
  [string]$Out,
  [string]$Bin,
  [string]$Preset = 'msvc-release',
  [int]$Width = 640,
  [int]$Height = 640,
  [double]$Orbit = 0,
  [int]$Frames = 4,
  [double]$CoarseLod = 1.0,
  [double]$FinestLod = 0.05,
  [ValidateSet('off', 'rt')] [string]$Shadows = 'off',
  # Capture without a window, through engine-host (see THE GPU above). Recorded in the report.
  [switch]$Offscreen,
  # The content build's atlas step (docs/subsystems/atlas.md): keep the generator's UV atlas, or
  # re-chart and rebake it (`engine-content build --atlas repack`). Recorded in the report.
  [ValidateSet('keep', 'repack')] [string]$Atlas = 'keep',
  # More `engine-content build` flags, verbatim (one string, split on spaces), for a measurement
  # that needs a builder setting the harness has no switch for — `--uv-seams none --uv-weight 0`
  # is the build whose LOD the atlas does not constrain at all, the ceiling any atlas could reach.
  # Recorded in the report.
  [string]$BuildArgs = '',
  # With -Atlas repack: also build each asset with the source's atlas and capture both containers'
  # finest cut from ONE camera (the kept build's framing) through engine-host, and compare them
  # (`reference` in the row) — which is how the rebake is judged: the two should agree except at
  # chart borders. Two engine-view runs cannot do it: engine-view frames the union of the leaf
  # clusters' spheres, and a different atlas makes different clusters, so the same mesh is framed a
  # little differently by each build.
  [switch]$CompareKept,
  [double]$MaxFlip = 0.02,
  [double]$MinIslandTexels = 1.0,
  [int]$MaxWarnings = 0,
  [long]$DenseTriangles = 250000,
  [double]$LowCoverage = 0.15,
  [double]$MaxCollapseShare = 0.75,
  [double]$MinCollapseDensity = 3.0,
  [switch]$AllowStale,
  [switch]$CheckBinaries,
  # What the three binaries' numbers are made of, for the staleness check: the three apps, the
  # import and cluster build, the renderer, and two that move the numbers just as surely —
  # domain/gfx (the shaders and passes that draw the picture) and foundation/image (FLIP, PSNR and
  # SSIM themselves).
  [string[]]$StalenessPaths = @('apps/engine_content', 'apps/engine_view', 'apps/engine_image', 'domain/geometry', 'domain/assets',
    'domain/atlas', 'systems/renderer', 'domain/gfx', 'foundation/image'),
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

# Why the collapse line is where it is (THE THRESHOLDS; docs/experiments/e10-generated-props.md, "The
# collapse check"), for report.json and report.md.
$script:CollapseWhy = 'E10 calibration at 640x640, orbit 16 and fit, 1 px against 0.05 px: known-good Khronos samples judged draw at most 46% (FlightHelmet), at orbit 16 every generated mesh not made by the triangle remesh at most 68%, the six Meshy triangle remeshes that do not coarsen 86-100%; below 3 pairs per 1,000 object pixels a mesh is no denser than the view can show (SciFiHelmet: 1.3 and 2.3)'

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
function ConvertTo-Ordered($o) {
  $h = [ordered]@{}
  foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = $p.Value }
  return $h
}
function Get-LockStatus {
  try { return ((& (Join-Path $PSScriptRoot 'gpu-lock.ps1') status *>&1 | Out-String).Trim()) } catch { return 'unknown' }
}

# ---- stale binaries ------------------------------------------------------------------------------------

function Invoke-Git([string[]]$gitArgs) {
  # No git (a container that syncs the tree without it) is "cannot say", never a crash.
  try { $out = & git -C $RepoRoot @gitArgs 2>$null; $ok = $LASTEXITCODE -eq 0 } catch { $out = @(); $ok = $false }
  return [pscustomobject]@{ Ok = $ok; Lines = [string[]]@($out | Where-Object { $null -ne $_ -and "$_" -ne '' } | ForEach-Object { "$_" }) }
}

# The build stamp a binary carries (`--version`: {"tool","commit","dirty"}), from the copy — never
# the build tree's binary in place. A binary from before stamps existed answers with its usage and
# exit 2, and gets $null.
function Get-BuildStamp([string]$exe) {
  try {
    $lines = & $exe --version 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    $j = Get-LastJsonLine $lines
    if ($j -and $j.PSObject.Properties['commit']) { return [ordered]@{ commit = [string]$j.commit; dirty = [bool]$j.dirty } }
  } catch { }
  return $null
}

# Each binary against the checkout (see STALE BINARIES in the help). Adds stamp, judged_by, stale
# and stale_reasons to every binary record, and returns what the report says about the checkout.
function Test-StaleBinaries($binaries) {
  $head = (Invoke-Git @('log', '-1', '--format=%H%x09%cI', 'HEAD')).Lines
  $last = (Invoke-Git (@('log', '-1', '--format=%H%x09%cI', '--') + $StalenessPaths)).Lines
  $status = (Invoke-Git (@('status', '--porcelain', '--') + $StalenessPaths)).Lines
  $headCommit = $null; $headTime = $null; $lastCommit = $null; $lastTime = $null
  if ($head.Count) { $p = $head[0] -split "`t"; $headCommit = $p[0]; $headTime = [DateTimeOffset]::Parse($p[1], [Globalization.CultureInfo]::InvariantCulture).UtcDateTime }
  if ($last.Count) { $p = $last[0] -split "`t"; $lastCommit = $p[0]; $lastTime = [DateTimeOffset]::Parse($p[1], [Globalization.CultureInfo]::InvariantCulture).UtcDateTime }
  $dirty = @()
  foreach ($line in $status) {
    $f = "$line".Substring(3); if ($f -match ' -> ') { $f = ($f -split ' -> ')[-1] }
    $full = Join-Path $RepoRoot $f.Trim('"')
    if (Test-Path -LiteralPath $full -PathType Leaf) { $dirty += [pscustomobject]@{ path = $f; modified = (Get-Item -LiteralPath $full).LastWriteTimeUtc } }
  }
  foreach ($b in $binaries) {
    $reasons = @()
    $built = [DateTime]::Parse($b.built, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
    $stamp = Get-BuildStamp $b.copy
    $b.stamp = $stamp
    $known = $stamp -and $stamp.commit -match '^[0-9a-f]{40}$' -and (Invoke-Git @('cat-file', '-e', "$($stamp.commit)^{commit}")).Ok
    if ($known) {
      $b.judged_by = 'build stamp'
      $changed = (Invoke-Git (@('diff', '--name-only', $stamp.commit, 'HEAD', '--') + $StalenessPaths)).Lines
      if ($changed.Count -gt 0) {
        $reasons += ("built from {0}, and {1} file(s) it is made of changed between that and HEAD {2} (first: {3})" -f $stamp.commit.Substring(0, 12), $changed.Count, "$headCommit".Substring(0, [math]::Min(12, "$headCommit".Length)), $changed[0])
      }
    } else {
      $b.judged_by = 'modification time (no build stamp)'
      if ($lastTime -and $built -lt $lastTime) {
        $reasons += ("built {0:u}, before {1} ({2:u}) changed what it is made of; it has no build stamp to say more" -f $built, "$lastCommit".Substring(0, 12), $lastTime)
      }
    }
    foreach ($d in $dirty) {
      if ($d.modified -gt $built) { $reasons += ("the working tree's {0} was edited after it was built ({1:u} > {2:u})" -f $d.path, $d.modified, $built) }
    }
    $b.stale = $reasons.Count -gt 0
    $b.stale_reasons = $reasons
  }
  return [ordered]@{
    head = $headCommit; head_time = $(if ($headTime) { $headTime.ToString('o') } else { $null })
    last_relevant_commit = $lastCommit; last_relevant_time = $(if ($lastTime) { $lastTime.ToString('o') } else { $null })
    staleness_paths = $StalenessPaths
    working_tree_edits = @($dirty | ForEach-Object { $_.path })
    built_from = @($binaries | Where-Object { $_.stamp } | ForEach-Object { $_.stamp.commit } | Sort-Object -Unique)
    stale = @($binaries | Where-Object { $_.stale }).Count -gt 0
    allow_stale = [bool]$AllowStale
  }
}

# The nearest orbit (engine-view's `--orbit`, tenths of the bounding radius) at which the whole
# bounding sphere is in frame: the camera sits `orbit/10` radii out on the ground plane and 0.45 of
# that above (systems/renderer view_set.cpp, orbit_camera), with a 55° vertical field of view and
# the horizontal one the aspect makes of it. A sphere of radius r at distance D fits when
# r/D <= sin(half the narrower field of view); the 5% margin keeps a silhouette off the edge.
function Get-FitOrbit([int]$w, [int]$h) {
  $halfV = 27.5 * [math]::PI / 180
  $half = [math]::Atan([math]::Tan($halfV) * [math]::Min(1.0, $w / [double]$h))
  $raw = 10 * 1.05 / ([math]::Sqrt(1 + 0.45 * 0.45) * [math]::Sin($half))
  return [math]::Ceiling($raw * 10) / 10
}

# The object's share of the frame: engine-view clears to one flat sky colour, so every pixel that is
# not the corner pixel's colour is the object. Windows only (System.Drawing); null elsewhere.
# Two containers drawn from ONE camera through engine-host --stdio (`render.load` twice, then
# `render.capture` twice with an explicit camera): the first container's orbit framing, computed
# the way engine-view computes it for frame `Frame` (systems/renderer view_set.cpp,
# `orbit_camera`), used for both. Returns the two PNG paths. The host reads one request per line
# ending in LF — a PowerShell pipe ends lines in CRLF, which it refuses — so the requests are
# written through the process's own stream.
function Invoke-OneCamera([string]$HostExe, [string]$First, [string]$Second, [string]$OutDir, [double]$Distance, [double]$Lod, [int]$Frame) {
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = $HostExe
  $psi.Arguments = '--stdio'
  $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  $psi.UseShellExecute = $false
  $process = [Diagnostics.Process]::Start($psi)
  $null = $process.StandardError.ReadToEndAsync()
  $process.StandardInput.NewLine = "`n"
  $script:rpcId = 0
  $call = {
    param([string]$method, $params)
    $script:rpcId++
    $process.StandardInput.WriteLine(([ordered]@{ jsonrpc = '2.0'; id = $script:rpcId; method = $method; params = $params } | ConvertTo-Json -Depth 10 -Compress))
    $process.StandardInput.Flush()
    $response = $process.StandardOutput.ReadLine() | ConvertFrom-Json
    if ($response.error) { throw "$method failed: $($response.error.message)" }
    $response.result
  }
  try {
    # The two orbiting point lights off (`lights = false`): the renderer places them from the scene's
    # bounds (systems/renderer lighting.cpp: 1.35 radii from the centre, a range of 4 radii), and
    # the bounds are the union of the leaf clusters' spheres, which differ between the two builds
    # exactly as the framing would. With them on, a repack whose textures matched to a tenth of a
    # level still drew 2-11 levels brighter or darker overall. The sun is a direction, the same for
    # both, so the pictures are still lit and still shaded by the normal maps.
    $settings = [ordered]@{ lod_px = $Lod; shadows = 'off'; raster = 'hw'; lights = $false }
    $sceneOne = & $call 'render.load' ([ordered]@{ mesh = ($First -replace '\\', '/'); settings = $settings; cache = $false })
    $sceneTwo = & $call 'render.load' ([ordered]@{ mesh = ($Second -replace '\\', '/'); settings = $settings; cache = $false })
    $c = @($sceneOne.center); $r = [double]$sceneOne.radius
    $d = $Distance * ($r / 10.0); $angle = $Frame * 0.006
    $camera = [ordered]@{ position = @(([double]$c[0] + [math]::Cos($angle) * $d), ([double]$c[1] + 0.45 * $d), ([double]$c[2] + [math]::Sin($angle) * $d))
      target = @([double]$c[0], [double]$c[1], [double]$c[2]); fov_deg = 55; znear = 0.01 * $r }
    $dirText = $OutDir -replace '\\', '/'
    $null = & $call 'render.capture' ([ordered]@{ scene = [string]$sceneOne.scene; camera = $camera; width = $Width; height = $Height; out_dir = $dirText; name = 'one-camera-first'; frame = $Frame })
    $null = & $call 'render.capture' ([ordered]@{ scene = [string]$sceneTwo.scene; camera = $camera; width = $Width; height = $Height; out_dir = $dirText; name = 'one-camera-second'; frame = $Frame })
  } finally {
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(60000)) { $process.Kill() }
  }
  return [pscustomobject]@{ first = (Join-Path $OutDir 'one-camera-first.png'); second = (Join-Path $OutDir 'one-camera-second.png') }
}

# -Offscreen: the two cuts of one container drawn through engine-host --stdio, which draws
# offscreen, from the orbit camera engine-view would use for frame `Frame` (the same arithmetic as
# Invoke-OneCamera). `engine-view --offscreen` cannot stand in: its plain offscreen summary carries
# no visible-pair count, and the collapse check is judged on exactly that, while `render.capture`
# returns the frame's `stats.visible_pairs`. Returns one visible-pair count per cut, in order; the
# PNGs are <OutDir>/<tag>.png as engine-view's would be.
function Invoke-OffscreenCuts([string]$HostExe, [string]$Clusters, [string]$OutDir, [double]$Distance, $Cuts, [int]$Frame, [string]$ShadowMode) {
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = $HostExe
  $psi.Arguments = '--stdio'
  $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  $psi.UseShellExecute = $false
  $process = [Diagnostics.Process]::Start($psi)
  $null = $process.StandardError.ReadToEndAsync()
  $process.StandardInput.NewLine = "`n"
  $script:rpcId = 0
  $call = {
    param([string]$method, $params)
    $script:rpcId++
    $process.StandardInput.WriteLine(([ordered]@{ jsonrpc = '2.0'; id = $script:rpcId; method = $method; params = $params } | ConvertTo-Json -Depth 10 -Compress))
    $process.StandardInput.Flush()
    $response = $process.StandardOutput.ReadLine() | ConvertFrom-Json
    if ($response.error) { throw "$method failed: $($response.error.message)" }
    $response.result
  }
  $pairs = @()
  try {
    $camera = $null
    foreach ($cut in $Cuts) {
      $settings = [ordered]@{ lod_px = [double]$cut.lod; shadows = $ShadowMode; raster = 'hw' }
      $scene = & $call 'render.load' ([ordered]@{ mesh = ($Clusters -replace '\\', '/'); settings = $settings; cache = $false })
      if (-not $camera) {
        $c = @($scene.center); $r = [double]$scene.radius
        $d = $Distance * ($r / 10.0); $angle = $Frame * 0.006
        $camera = [ordered]@{ position = @(([double]$c[0] + [math]::Cos($angle) * $d), ([double]$c[1] + 0.45 * $d), ([double]$c[2] + [math]::Sin($angle) * $d))
          target = @([double]$c[0], [double]$c[1], [double]$c[2]); fov_deg = 55; znear = 0.01 * $r }
      }
      $result = & $call 'render.capture' ([ordered]@{ scene = [string]$scene.scene; camera = $camera; width = $Width; height = $Height; out_dir = ($OutDir -replace '\\', '/'); name = $cut.tag; frame = $Frame })
      $pairs += [long]$result.stats.visible_pairs
    }
  } finally {
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(60000)) { $process.Kill() }
  }
  return $pairs
}

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

# ---- judging: thresholds and diagnoses, from the measurements alone ---------------------------------

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
    $fix = if ($RuleRepairs.ContainsKey([string]$row.build_rule)) { $RuleRepairs[[string]$row.build_rule] } else { 'see the build error' }
    return "import refused ($($row.build_rule)): $fix"
  }
  $parts = @()
  if ($row.checks.warnings -eq 'fail') {
    $rules = @($row.warning_rules.PSObject.Properties | ForEach-Object { "$($_.Name) x$($_.Value)" }) -join ', '
    $fixes = @($row.warning_rules.PSObject.Properties.Name | ForEach-Object { if ($RuleRepairs.ContainsKey($_)) { $RuleRepairs[$_] } }) -join '; '
    $parts += "$($row.warnings) warnings ($rules): $fixes"
  }
  # Two different atlas defects fail the same threshold, and their repairs differ by an order of
  # magnitude: a few triangles whose UVs coincide or nearly do (an island of no area — a UV-hygiene
  # fault, which the content build now folds into the neighbouring island itself, as the repair
  # `geometry.uv_degenerate`) and a genuinely fragmented atlas (a repack and a rebake). E10's first
  # pass found the first in most of its failures and the second in a few, so the diagnosis says
  # which, by the atlas's median island and its seam share. After the repair, a sub-texel island
  # that is still there is one it could not reach.
  $fragmented = ($null -ne $row.median_island_texels_4096 -and $row.median_island_texels_4096 -lt 1024) -or ($null -ne $row.seam_fraction -and $row.seam_fraction -gt 0.3)
  if ($row.checks.island -eq 'fail') {
    if ($fragmented) {
      $parts += ("fragmented atlas: {0:N0} islands, median {1:N0} texels, smallest {2:0.###} over {3} triangles, {4:0.0}% of vertices on a seam — repack into fewer, larger islands and rebake the textures; the geometry builder cannot help" -f $row.islands, $row.median_island_texels_4096, $row.smallest_island_texels_4096, $row.smallest_island_triangles, (100 * $row.seam_fraction))
    } else {
      $folded = @($row.repairs | Where-Object { $_ -and $_.rule -eq 'geometry.uv_degenerate' }) | Select-Object -First 1
      $by = if ($folded) { " the build's UV repair folded {0:N0} such islands ({1:N0} triangles) and left {2:N0} triangles with no sound island beside them, which is where this one is —" -f [long]$folded.islands, [long]$folded.triangles, [long]$folded.unrepaired } else { '' }
      $parts += ("sub-texel island in a sound atlas: the smallest of {0:N0} islands (median {1:N0} texels) is {2} triangle(s) with {3:0.###} texels of UV area;{4} fold those triangles' UVs into a neighbouring island by hand, or drop the floating part (a UV cleanup: no repack, no rebake)" -f $row.islands, $row.median_island_texels_4096, $row.smallest_island_triangles, $row.smallest_island_texels_4096, $by)
    }
  }
  if ($row.checks.flip -eq 'fail') {
    $why = @()
    if ($row.triangles -gt $DenseTriangles) {
      $why += ("dense ({0:N0} triangles; {1:N0} pairs at the finest cut against {2:N0} at the coarse): the detail is in the geometry, and a cut that keeps positions within a pixel still changes the shading and drops small features — decimate to the category's budget and bake the detail into the normal map" -f $row.triangles, $row.finest_visible_pairs, $row.coarse_visible_pairs)
    }
    if ($fragmented) {
      $why += ("fragmented atlas ({0:N0} islands, median {1:N0} texels, {2:0.0}% seam vertices): coarse levels keep the seams and smear between them — repack or bake per-level textures" -f $row.islands, $row.median_island_texels_4096, (100 * $row.seam_fraction))
    }
    if ($why.Count -eq 0) { $why += 'neither dense nor fragmented: read the heat map (thin parts collapsing, or a normal-map seam)' }
    $parts += ("coarse cut is FLIP {0:0.0000} / PSNR {1:0.0} dB from the finest ({2:0.000} over the object's {3:0}% of the frame); {4}" -f $row.flip_mean, $row.psnr, $row.flip_object_mean, (100 * [double]$row.coverage), ($why -join '; and '))
  }
  if ($row.checks.flip -eq 'unmeasured') { $parts += 'no picture: engine-view could not render here (exit 3)' }
  # A mesh whose LOD does not collapse passes the picture check for the wrong reason: the coarse cut
  # is the finest cut, so it has nothing to lose, and the mesh costs its finest cut at every distance.
  # E10's second pass found the cause in the atlas — a seam vertex is one the simplifier may not
  # collapse across (geometry.md), and the share followed the seam fraction at r = 0.94 — so the
  # diagnosis leads with the atlas when it is fragmented, and with the geometry when it is not.
  if ($row.checks.collapse -eq 'fail') {
    $what = "the LOD does not collapse: at the default threshold ({0} px) the coarse cut still draws {1:0}% of the finest cut's pairs ({2:N0} of {3:N0}), so the mesh costs close to its finest cut at every distance, and its FLIP ({4:0.0000}) passes for want of anything to lose" -f `
      $script:JudgedCoarsePx, (100 * [double]$row.collapse_share), [long]$row.coarse_visible_pairs, [long]$row.finest_visible_pairs, [double]$row.flip_mean
    if ($fragmented) {
      $what += ("; the atlas is why ({0:N0} islands, median {1:N0} texels, {2:0.0}% of vertices on a seam, and the simplifier may not collapse across a seam) — ask the generator for a sounder atlas (Meshy's quad remesh, TRELLIS.2's pec segmenter) or repack and rebake" -f $row.islands, $row.median_island_texels_4096, (100 * $row.seam_fraction))
    } else {
      $levels = if ($row.PSObject.Properties['level_clusters'] -and $row.level_clusters) { " (clusters per level, leaves first: $(@($row.level_clusters) -join ', '))" } else { '' }
      $what += "; the atlas is not fragmented, so the geometry is why: detail at the scale of the view that any simplification moves by more than the threshold (a rope's strands — decimate and bake it into the normal map), many separate parts, or normals split along every edge; read the heat map and the DAG$($levels)"
    }
    $parts += $what
  }
  if ($row.checks.collapse -eq 'unmeasured') { $parts += 'no visible pairs recorded: the LOD collapse could not be judged' }
  return ($parts -join ' | ')
}

# Checks, status and diagnosis, recomputed from the stored measurements every time.
function Set-Verdict($row) {
  $checks = [ordered]@{ import = 'pass'; warnings = 'pass'; island = 'pass'; flip = 'pass'; collapse = 'pass' }
  # The share is computed at judging time from the two counts every report since the first carries,
  # so -FromReport judges a report measured before the check existed.
  $coarse = if ($row.Contains('coarse_visible_pairs')) { $row.coarse_visible_pairs } else { $null }
  $finest = if ($row.Contains('finest_visible_pairs')) { $row.finest_visible_pairs } else { $null }
  $row.collapse_share = if ($null -ne $coarse -and $null -ne $finest -and [long]$finest -gt 0) { [math]::Round([double]$coarse / [double]$finest, 4) } else { $null }
  # How dense the finest cut is on screen: its pairs per thousand pixels of the object. Below
  # -MinCollapseDensity the mesh is already no denser than the view can show, and not coarsening
  # there is not a fault (THE THRESHOLDS).
  $row.finest_pairs_per_kpx = if ($null -ne $finest -and $null -ne $row.coverage -and [double]$row.coverage -gt 0) { [math]::Round(1000 * [double]$finest / ([double]$row.coverage * $script:JudgedPixels), 3) } else { $null }
  if ($row.Contains('build_rule') -and $row.build_rule) {
    $checks.import = 'fail'; $checks.warnings = 'n/a'; $checks.island = 'n/a'; $checks.flip = 'n/a'; $checks.collapse = 'n/a'
  } else {
    if ([int]$row.warnings -gt $MaxWarnings) { $checks.warnings = 'fail' }
    if ($null -eq $row.smallest_island_texels_4096) { $checks.island = 'unmeasured' }
    elseif ([double]$row.smallest_island_texels_4096 -lt $MinIslandTexels) { $checks.island = 'fail' }
    if ($null -eq $row.flip_mean) { $checks.flip = 'unmeasured' }
    elseif ([double]$row.flip_mean -gt $MaxFlip) { $checks.flip = 'fail' }
    # -MaxCollapseShare, judged where the finest cut is at least -MinCollapseDensity (THE THRESHOLDS).
    # With no coverage (off Windows the harness cannot read a capture's pixels) there is no density,
    # and every mesh is judged on its share alone.
    if ($null -eq $row.collapse_share) { $checks.collapse = if ($null -eq $row.flip_mean) { 'n/a' } else { 'unmeasured' } }
    elseif ($null -ne $row.finest_pairs_per_kpx -and [double]$row.finest_pairs_per_kpx -lt $MinCollapseDensity) { $checks.collapse = 'n/a' }
    elseif ([double]$row.collapse_share -gt $MaxCollapseShare) { $checks.collapse = 'fail' }
  }
  $row.checks = $checks
  # Not a check: whether the object-only FLIP means anything for this asset (-LowCoverage).
  $row.object_flip_reliable = if ($null -eq $row.coverage -or $null -eq $row.flip_object_mean) { $null } else { [double]$row.coverage -ge $LowCoverage }
  $failed = @($checks.Keys | Where-Object { $checks[$_] -in 'fail', 'unmeasured' })
  $row.status = if ($failed.Count -eq 0) { 'pass' } else { 'fail' }
  $row.diagnosis = if ($failed.Count -eq 0) { '' } else { Get-Diagnosis ([pscustomobject]$row) }
}

# ---- measuring ---------------------------------------------------------------------------------------

$rows = @()
# What the verdicts are judged against: the settings the captures were made with (a re-judged
# report's own, which -Width, -Height and -CoarseLod do not change).
$script:JudgedCoarsePx = $CoarseLod
$script:JudgedPixels = [double]$Width * $Height
if ($FromReport) {
  $old = Get-Content -Raw -LiteralPath $FromReport | ConvertFrom-Json
  if ($old.schema -ne 'engine.e10.report/1') { throw "$FromReport is not an engine.e10.report/1 file" }
  $Out = Split-Path -Parent (Resolve-Path -LiteralPath $FromReport).Path
  if ($old.settings -and $null -ne $old.settings.coarse_lod_px) { $script:JudgedCoarsePx = $old.settings.coarse_lod_px }
  if ($old.settings -and $old.settings.width -and $old.settings.height) { $script:JudgedPixels = [double]$old.settings.width * $old.settings.height }
  foreach ($a in @($old.assets)) {
    $row = ConvertTo-Ordered $a
    foreach ($k in @('checks', 'status', 'diagnosis', 'object_flip_reliable', 'collapse_share', 'finest_pairs_per_kpx')) { if ($row.Contains($k)) { $row.Remove($k) } }
    Set-Verdict $row
    $rows += [pscustomobject]$row
  }
  $folders = @($old.folders)
  $toolsBlock = $old.tools
  $settingsBlock = $old.settings
  $machine = $old.machine_state
  # ConvertFrom-Json turns an ISO 8601 string into a DateTime, which [string] then prints in the
  # local culture's format; write it back the way it was read.
  $iso = { param($v) if ($v -is [DateTime]) { $v.ToUniversalTime().ToString('o') } else { [string]$v } }
  $startedText = & $iso $old.started_utc; $finishedText = & $iso $old.finished_utc; $seconds = $old.seconds
  if (-not $Title) { $Title = [string]$old.title }
  Write-Log "e10: re-judged $($rows.Count) assets from $FromReport under the thresholds given"
} else {
  if ($CheckBinaries) {
    if (-not $Out) { throw '-CheckBinaries copies the binaries before asking them anything; give -Out <scratch dir>' }
    $folders = @()
  } else {
    if (-not $Folder) { throw 'give -Folder <dir of .glb or .gltf> (or -FromReport <report.json> to re-judge one)' }
    $folders = @()
    foreach ($f in $Folder) { foreach ($p in ($f -split ',')) { if ($p.Trim()) { $folders += (Resolve-Path -LiteralPath $p.Trim()).Path } } }
    $glbs = @()
    # A .gltf beside its buffers and images is taken as readily as a .glb, so the Khronos samples
    # the collapse limit was calibrated on (content/samples) go through the same harness.
    foreach ($f in $folders) { $glbs += @(Get-ChildItem -LiteralPath $f -File | Where-Object { $_.Extension -in '.glb', '.gltf' } | Sort-Object Name) }
    if ($Only) {
      $wanted = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
      $glbs = @($glbs | Where-Object { $wanted -contains $_.BaseName })
    }
    if ($glbs.Count -eq 0) { throw "no .glb or .gltf files in $($folders -join ', ')" }
  }

  if (-not $Out) {
    $root = if ($LocalRoot) { $LocalRoot } elseif ($env:ENGINE_LOCAL_ROOT) { $env:ENGINE_LOCAL_ROOT } elseif ($IsWin) { 'D:\workspace\game_engine_local' } else { Join-Path $HOME 'game_engine_local' }
    $leaf = Split-Path -Leaf $folders[0]; $parent = Split-Path -Leaf (Split-Path -Parent $folders[0])
    $Out = Join-Path (Join-Path $root 'e10') "$parent-$leaf"
  }
  $Out = [IO.Path]::GetFullPath($Out)
  $repoPrefix = $RepoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if ($Out.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "-Out '$Out' is inside the repository; captures and containers of generated assets are never committed" }
  New-Item -ItemType Directory -Force -Path (Join-Path $Out 'bin'), (Join-Path $Out 'assets') | Out-Null

  # The binaries: copies, never the build tree in place.
  $source = if ($Bin) { (Resolve-Path -LiteralPath $Bin).Path } else { Join-Path $RepoRoot "build/$Preset/bin" }
  $binaries = @()
  # engine-host only for -CompareKept and -Offscreen, whose captures go through the render protocol.
  foreach ($t in @('engine-content', 'engine-view', 'engine-image') + @(if ($CompareKept -or $Offscreen) { 'engine-host' })) {
    $src = Join-Path $source "$t$Exe"
    if (-not (Test-Path -LiteralPath $src)) { throw "no $t$Exe in $source. Build it (tools/dev.ps1 build -Preset $Preset) or pass -Bin <dir>." }
    Copy-Item -LiteralPath $src -Destination (Join-Path $Out 'bin') -Force
    $copy = Join-Path (Join-Path $Out 'bin') "$t$Exe"
    $binaries += [ordered]@{ name = "$t$Exe"; sha256 = Get-FileSha256 $copy; built = (Get-Item -LiteralPath $src).LastWriteTimeUtc.ToString('o'); copy = $copy
      stamp = $null; judged_by = $null; stale = $false; stale_reasons = @() }
  }
  Get-ChildItem -LiteralPath $source -File -Filter '*.dll' -ErrorAction SilentlyContinue | Copy-Item -Destination (Join-Path $Out 'bin') -Force
  $content = Join-Path (Join-Path $Out 'bin') "engine-content$Exe"
  $view = Join-Path (Join-Path $Out 'bin') "engine-view$Exe"
  $image = Join-Path (Join-Path $Out 'bin') "engine-image$Exe"
  $commit = ''
  try { $commit = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim() } catch { }
  $freshness = Test-StaleBinaries $binaries
  foreach ($b in $binaries) {
    $what = if ($b.stamp) { "built from $($b.stamp.commit.Substring(0, [math]::Min(12, $b.stamp.commit.Length)))$(if ($b.stamp.dirty) { ' (dirty tree)' })" } else { "no build stamp, built $($b.built)" }
    Write-Log "e10: $($b.name): $what$(if ($b.stale) { ' — STALE' })"
  }
  if ($CheckBinaries) {
    # The check alone, as one JSON line: exit 0 fresh, 3 stale (what tools/e10-harness.Tests.ps1 reads).
    foreach ($b in $binaries) { $b.Remove('copy') }
    [Console]::Out.WriteLine(([ordered]@{ source = $source; commit = $commit; freshness = $freshness; binaries = $binaries } | ConvertTo-Json -Depth 8 -Compress))
    exit $(if ($freshness.stale) { 3 } else { 0 })
  }
  if ($freshness.stale) {
    $lines = @($binaries | Where-Object { $_.stale } | ForEach-Object { "  $($_.name) ($source): $($_.stale_reasons -join '; ')" })
    $rebuild = if ($Bin) { "rebuild the tree $source came from" } else { "pwsh tools/dev.ps1 build -Preset $Preset" }
    if (-not $AllowStale) {
      throw ("refusing to measure with stale binaries — the report would name commit $("$commit".Substring(0, [math]::Min(12, "$commit".Length))) for code older than it:`n" + ($lines -join "`n") +
        "`nRebuild them ($rebuild), or pass -AllowStale to measure anyway with a STALE BINARIES caveat in the report.")
    }
    Write-Log "e10: WARNING: measuring with stale binaries (-AllowStale); the report says so at the top:"
    foreach ($l in $lines) { Write-Log $l }
  }

  if ($CompareKept -and $Atlas -eq 'keep') { throw '-CompareKept compares a repacked build with a kept one; give -Atlas repack' }
  $hostExe = Join-Path (Join-Path $Out 'bin') "engine-host$Exe"
  $orbitUsed = if ($Orbit -gt 0) { $Orbit } else { Get-FitOrbit $Width $Height }
  $framing = if ($Orbit -gt 0) { "fixed: orbit $Orbit (-Orbit)" } else { "fit: the whole bounding sphere in frame with a 5% margin (orbit $orbitUsed)" }
  Write-Log "e10: framing $framing"

  $started = [DateTime]::UtcNow
  $lockAtStart = Get-LockStatus
  Write-Log "e10: $($glbs.Count) GLBs from $($folders -join ', ')"
  Write-Log "e10: binaries copied from $source (commit $commit) into $(Join-Path $Out 'bin')"
  Write-Log "e10: GPU lock at start: $lockAtStart (captures do not take it)"

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
      # A comfyui-3d sidecar also says how the mesh was post-processed and how long that took,
      # which is what a parameter sweep is read against.
      if ($s.PSObject.Properties['timings'] -and $s.parameters -and $s.parameters.PSObject.Properties['decimate']) {
        $row.generation = [ordered]@{
          face_count = $s.parameters.decimate.target_face_count; segmenter = $s.parameters.segmenter.value
          smooth_iters = $s.parameters.remesh.smooth_iters; weld_distance = $s.parameters.unwrap.weld_distance; texture = $s.parameters.texture_resolution
          unwrap_seconds = $s.timings.unwrap_seconds; bake_seconds = $s.timings.bake_seconds; wall_seconds = $s.timings.wall_seconds; cached_nodes = @($s.timings.cached_nodes).Count
          cpu_pct = $s.machine_state.cpu_pct; gpu_util_pct = $s.machine_state.gpu_util_pct; comfyui_cores_busy = $s.machine_state.comfyui_cores_busy
          # An image-in run (`-Images`) was given its picture rather than drawing it: say so, and which.
          image_supplied = $(if ($s.parameters.PSObject.Properties['image'] -and $s.parameters.image) { [bool]$s.parameters.image.supplied } else { $false })
        }
      }
    } else {
      $row.service = Split-Path -Leaf (Split-Path -Parent $glb.DirectoryName)
    }

    # 1. build
    $clusters = Join-Path $dir "$name.clusters"
    # `--atlas` only when it is not the default, so an older binary (-AllowStale) still builds.
    $atlasArgs = if ($Atlas -ne 'keep') { @('--atlas', $Atlas) } else { @() }
    $extraArgs = @($BuildArgs -split ' ' | Where-Object { $_ })
    $buildOut = & $content build $glb.FullName $clusters @atlasArgs @extraArgs 2> (Join-Path $dir 'build.err')
    $buildCode = $LASTEXITCODE
    $buildErr = "$(Get-Content -Raw -LiteralPath (Join-Path $dir 'build.err') -ErrorAction SilentlyContinue)"
    $b = Get-LastJsonLine $buildOut
    $rules = [ordered]@{}
    foreach ($m in [regex]::Matches($buildErr, '\b((?:geometry|material)\.[a-z_]+)\b')) {
      $k = $m.Groups[1].Value; if ($rules.Contains($k)) { $rules[$k]++ } else { $rules[$k] = 1 }
    }
    if ($buildCode -ne 0 -or -not $b) {
      $first = @($rules.Keys)[0]
      $row.build_rule = if ($first) { $first } else { "exit $buildCode" }
      $row.build_error = ([string]$buildErr).Trim().Split("`n")[0]
      Set-Verdict $row
      $rows += [pscustomobject]$row
      Write-Log "  import refused: $($row.build_error)"
      continue
    }
    $row.triangles = [long]$b.triangles; $row.clusters = [long]$b.clusters; $row.lod_levels = [int]$b.lod_levels
    $row.materials = [int]$b.materials; $row.images = [int]$b.images
    $row.warnings = [int]$b.warnings; $row.warning_rules = [pscustomobject]$rules
    $row.container_bytes = [long]$b.bytes; $row.build_ms = [math]::Round([double]$b.build_ms, 1)
    # What the build repaired rather than warned about (geometry.md, "UV-degenerate triangles:
    # the repair"): an empty array when nothing needed it, absent from a build older than the rule.
    if ($null -ne $b.repairs) { $row.repairs = @($b.repairs) }
    # The atlas step (atlas.md): charts, atlas sizes, and how far each rebaked image is from its
    # source on the surface. Absent from a build older than the flag, {"mode":"keep"} without it.
    if ($null -ne $b.atlas -and $b.atlas.mode -eq 'repack') {
      $colour = @($b.atlas.images | Where-Object { $_.space -ne 'normal' })
      $normal = @($b.atlas.images | Where-Object { $_.space -eq 'normal' })
      $row.atlas = [ordered]@{
        mode = 'repack'; charts = [long]$b.atlas.charts; materials = [int]$b.atlas.materials_repacked
        resolution = @($b.atlas.detail | ForEach-Object { [int]$_.resolution } | Measure-Object -Maximum).Maximum
        utilization = @($b.atlas.detail | ForEach-Object { [double]$_.utilization } | Measure-Object -Minimum).Minimum
        pack_attempts = @($b.atlas.detail | ForEach-Object { [int]$_.pack_attempts } | Measure-Object -Maximum).Maximum
        unatlased_triangles = @($b.atlas.detail | ForEach-Object { [long]$_.unatlased_triangles } | Measure-Object -Sum).Sum
        proxy_charts = @($b.atlas.detail | ForEach-Object { [long]$_.proxy_charts } | Measure-Object -Sum).Sum
        folded_triangles = @($b.atlas.detail | ForEach-Object { [long]$_.folded_triangles } | Measure-Object -Sum).Sum
        image_bytes = [long]$b.atlas.image_bytes; ms = $b.atlas.ms
        colour_error_mean = $(if ($colour.Count) { [math]::Round((@($colour | ForEach-Object { [double]$_.rebake_error.mean }) | Measure-Object -Maximum).Maximum, 3) } else { $null })
        colour_error_p99 = $(if ($colour.Count) { [math]::Round((@($colour | ForEach-Object { [double]$_.rebake_error.p99 }) | Measure-Object -Maximum).Maximum, 3) } else { $null })
        normal_error_mean_deg = $(if ($normal.Count) { [math]::Round((@($normal | ForEach-Object { [double]$_.rebake_error.mean }) | Measure-Object -Maximum).Maximum, 3) } else { $null })
        normal_error_p99_deg = $(if ($normal.Count) { [math]::Round((@($normal | ForEach-Object { [double]$_.rebake_error.p99 }) | Measure-Object -Maximum).Maximum, 3) } else { $null })
        crumb_charts = @($b.atlas.detail | ForEach-Object { [long]$_.crumb_charts } | Measure-Object -Sum).Sum
        supersample = $b.atlas.supersample; declined = [int]$b.atlas.materials_declined
        notes = @($b.atlas.notes)
      }
    }

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
      $row.lod_attribute_error = $st.lod_attribute_error
    }
    # Clusters per DAG level, leaves first: how far the build could collapse the mesh at all, whatever
    # the view (the collapse check below judges one view). Recorded, not judged.
    if ($st -and $null -ne $st.level_clusters) { $row.level_clusters = @($st.level_clusters | ForEach-Object { [long]$_ }) }

    # 3. two captures, of one frame, differing only in the LOD threshold
    $common = @('--mesh', $clusters, '--width', $Width, '--height', $Height, '--frames', $Frames, '--orbit', ([double]$orbitUsed).ToString($Inv), '--no-vsync', '--shadows', $Shadows)
    $picture = $true
    $cuts = @(@{ tag = 'coarse'; lod = $CoarseLod }, @{ tag = 'finest'; lod = $FinestLod })
    if ($Offscreen) {
      # No window: both cuts through engine-host (Invoke-OffscreenCuts), from engine-view's camera.
      try {
        $pairs = Invoke-OffscreenCuts -HostExe $hostExe -Clusters $clusters -OutDir $dir -Distance ([double]$orbitUsed) -Cuts $cuts -Frame ($Frames - 1) -ShadowMode $Shadows
        $row.coarse_visible_pairs = $pairs[0]
        $row.finest_visible_pairs = $pairs[1]
      } catch {
        if ("$_" -match 'no Vulkan|device') { $picture = $false } else { throw "engine-host failed on ${name}: $_" }
      }
      $cuts = @()
    }
    foreach ($cut in $cuts) {
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

    # 4. compare
    if ($picture) {
      $cmp = & $image compare --json (Join-Path $dir 'coarse.png') (Join-Path $dir 'finest.png') --flip (Join-Path $dir 'flip.png') 2> (Join-Path $dir 'compare.err') | ConvertFrom-Json
      $row.psnr = if ($null -eq $cmp.psnr) { $null } else { [math]::Round([double]$cmp.psnr, 2) }
      $row.ssim = [math]::Round([double]$cmp.ssim, 5)
      $row.flip_mean = [math]::Round([double]$cmp.flip_mean, 5)
      $row.flip_p95 = [math]::Round([double]$cmp.flip_p95, 5)
      $row.flip_max = [math]::Round([double]$cmp.flip_max, 4)
      $row.coverage = Get-Coverage (Join-Path $dir 'finest.png')
      $row.flip_object_mean = if ($row.coverage -gt 0) { [math]::Round($row.flip_mean / $row.coverage, 5) } else { $null }
      # -CompareKept: the source's atlas against the repacked one, at the finest cut, from one
      # camera — the rebake's test in the picture (see the parameter).
      if ($CompareKept) {
        $kept = Join-Path $dir "$name.kept.clusters"
        & $content build $glb.FullName $kept @extraArgs 2> (Join-Path $dir 'build-kept.err') | Out-Null
        if ($LASTEXITCODE -eq 0) {
          try {
            $pair = Invoke-OneCamera -HostExe $hostExe -First $kept -Second $clusters -OutDir $dir -Distance ([double]$orbitUsed) -Lod $FinestLod -Frame ($Frames - 1)
            $rc = & $image compare --json $pair.second $pair.first --flip (Join-Path $dir 'kept-flip.png') 2> (Join-Path $dir 'kept-compare.err') | ConvertFrom-Json
            $keptCoverage = Get-Coverage $pair.first
            $row.reference = [ordered]@{
              against = 'kept'; psnr = if ($null -eq $rc.psnr) { $null } else { [math]::Round([double]$rc.psnr, 2) }
              ssim = [math]::Round([double]$rc.ssim, 5); flip_mean = [math]::Round([double]$rc.flip_mean, 5)
              flip_p95 = [math]::Round([double]$rc.flip_p95, 5)
              flip_object_mean = if ($keptCoverage -gt 0) { [math]::Round([double]$rc.flip_mean / $keptCoverage, 5) } else { $null }
            }
          } catch { Write-Log "  one-camera comparison failed: $_" }
        }
        if (-not $KeepContainers) { Remove-Item -LiteralPath $kept -Force -ErrorAction SilentlyContinue }
      }
    }

    Set-Verdict $row
    if (-not $KeepContainers) { Remove-Item -LiteralPath $clusters -Force -ErrorAction SilentlyContinue }
    Write-Log ("  {0}: {1:N0} tri, {2} clusters, {3} levels, {4} warn, {5} islands, seam {6:P1}, smallest {7:0.###} texels, FLIP {8}, PSNR {9}" -f `
        $row.status, $row.triangles, $row.clusters, $row.lod_levels, $row.warnings, $row.islands, $row.seam_fraction, $row.smallest_island_texels_4096, $row.flip_mean, $row.psnr)
    $rows += [pscustomobject]$row
  }
  $finished = [DateTime]::UtcNow
  $lockAtEnd = Get-LockStatus
  $startedText = $started.ToString('o'); $finishedText = $finished.ToString('o')
  $seconds = [math]::Round(($finished - $started).TotalSeconds, 1)

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
  foreach ($b in $binaries) { $b.Remove('copy') }
  $toolsBlock = [ordered]@{ source = $source; commit = $commit; built_from = $freshness.built_from; preset = $(if ($Bin) { $null } else { $Preset }); binaries = $binaries
    freshness = $freshness; harness = 'tools/e10-harness.ps1' }
  $settingsBlock = [ordered]@{ width = $Width; height = $Height; orbit = $orbitUsed; framing = $framing; frames = $Frames; coarse_lod_px = $CoarseLod; finest_lod_px = $FinestLod; shadows = $Shadows; raster = 'hw (default)'
    atlas = $Atlas; build_args = $BuildArgs; compare_kept = [bool]$CompareKept; offscreen = [bool]$Offscreen }
  if (-not $Title) { $Title = "E10 over $((Split-Path -Leaf (Split-Path -Parent $folders[0])))/$(Split-Path -Leaf $folders[0])" }
}

# ---- totals ------------------------------------------------------------------------------------------

# Worst means the direction that hurts: most triangles, most warnings, smallest island, lowest PSNR.
$columns = [ordered]@{
  triangles = 'max'; clusters = 'max'; lod_levels = 'min'; warnings = 'max'; islands = 'max'; seam_fraction = 'max'
  smallest_island_texels_4096 = 'min'; container_bytes = 'max'; build_ms = 'max'; psnr = 'min'; flip_mean = 'max'
  coverage = 'min'; flip_object_mean = 'max'; collapse_share = 'max'
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

# What the picture error follows. A threshold says which assets fail; these say why a set fails —
# the first pass's answer was triangle count, not the atlas — and each later pass (another service,
# a remesh option) should be read the same way. Pearson over the measured assets, with the
# heavy-tailed counts on a log scale.
function Get-Pearson($xs, $ys) {
  $n = $xs.Count; if ($n -lt 3) { return $null }
  $mx = ($xs | Measure-Object -Average).Average; $my = ($ys | Measure-Object -Average).Average
  $sxy = 0.0; $sxx = 0.0; $syy = 0.0
  for ($i = 0; $i -lt $n; $i++) { $dx = $xs[$i] - $mx; $dy = $ys[$i] - $my; $sxy += $dx * $dy; $sxx += $dx * $dx; $syy += $dy * $dy }
  if ($sxx -eq 0 -or $syy -eq 0) { return $null }
  return [math]::Round($sxy / [math]::Sqrt($sxx * $syy), 2)
}
$measured = @($rows | Where-Object { $null -ne $_.flip_mean -and $null -ne $_.coverage -and $null -ne $_.islands })
$follows = [ordered]@{}
foreach ($spec in @(@{ k = 'log_triangles'; f = { [math]::Log([double]$args[0].triangles) } }, @{ k = 'log_islands'; f = { [math]::Log([double]$args[0].islands) } },
    @{ k = 'seam_fraction'; f = { [double]$args[0].seam_fraction } }, @{ k = 'log_median_island_texels'; f = { [math]::Log([math]::Max(1e-3, [double]$args[0].median_island_texels_4096)) } },
    @{ k = 'coverage'; f = { [double]$args[0].coverage } })) {
  $x = @($measured | ForEach-Object { & $spec.f $_ })
  $follows[$spec.k] = [ordered]@{
    flip_mean = Get-Pearson $x @($measured | ForEach-Object { [double]$_.flip_mean })
    flip_object_mean = Get-Pearson $x @($measured | ForEach-Object { [double]$_.flip_object_mean })
    # And what the LOD collapse follows: E10's second pass found the seam fraction (r = 0.94).
    collapse_share = $(if (@($measured | Where-Object { $null -eq $_.collapse_share }).Count -eq 0) { Get-Pearson $x @($measured | ForEach-Object { [double]$_.collapse_share }) } else { $null })
  }
}

$passed = @($rows | Where-Object status -eq 'pass').Count
$byCheck = [ordered]@{}
foreach ($k in @('import', 'warnings', 'island', 'flip', 'collapse')) { $byCheck[$k] = @($rows | Where-Object { $_.checks[$k] -in 'fail', 'unmeasured' }).Count }

$staleNames = @($toolsBlock.binaries | Where-Object { $_.stale } | ForEach-Object { $_.name })
$report = [ordered]@{
  schema = 'engine.e10.report/1'
  title = $Title
  # Second key, where a reader of the JSON meets it before any number (-AllowStale only).
  caveat = $(if ($staleNames.Count) { "STALE BINARIES: $($staleNames -join ', ') older than the source at commit $($toolsBlock.commit); measured with -AllowStale (tools.freshness has why)" } else { $null })
  started_utc = $startedText; finished_utc = $finishedText; seconds = $seconds
  judged_utc = [DateTime]::UtcNow.ToString('o')
  folders = $folders
  tools = $toolsBlock
  settings = $settingsBlock
  thresholds = [ordered]@{
    max_warnings = $MaxWarnings; min_island_texels_4096 = $MinIslandTexels; max_flip_mean = $MaxFlip; dense_triangles = $DenseTriangles
    max_collapse_share = $MaxCollapseShare; min_collapse_density_pairs_per_kpx = $MinCollapseDensity
    why = [ordered]@{
      warnings = 'an asset that needs a person to read a warning before use is not one the pipeline can take unattended'
      island = 'an island under one texel of a 4096 atlas cannot be sampled as itself and no seam-respecting simplification can coarsen it'
      flip = 'geometry.md seam-fix numbers: defective builds 0.0241 and 0.0223, fixed builds 0.0115 and 0.0083, FlightHelmet 0.0173; 0.02 separates them'
      dense = 'diagnosis only: above this a FLIP failure is read as detail carried by geometry'
      collapse = $script:CollapseWhy
    }
  }
  machine_state = $machine
  totals = [ordered]@{ assets = $rows.Count; passed = $passed; pass_rate = $(if ($rows.Count) { [math]::Round($passed / $rows.Count, 3) } else { 0 }); failed_by_check = $byCheck; columns = $colStats }
  flip_follows = $follows
  assets = $rows
}
Write-JsonFile (Join-Path $Out 'report.json') $report

# ---- report.md -----------------------------------------------------------------------------------------

function F($v, [string]$fmt) { if ($null -eq $v -or ($v -is [string] -and $v -eq '')) { return '—' }; return ([double]$v).ToString($fmt, $Inv) }
$commitText = [string]$toolsBlock.commit
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# $Title")
$md.Add('')
$fresh = $toolsBlock.freshness
if ($fresh -and $fresh.stale) {
  # First, before any number: a reader who stops after one line must still know.
  $md.Add('> **STALE BINARIES.** Measured with `-AllowStale`: at least one binary is older than the source the commit below names, so any number here may come from older code.')
  foreach ($b in @($toolsBlock.binaries | Where-Object { $_.stale })) { $md.Add("> - ``$($b.name)``: $(@($b.stale_reasons) -join '; ')") }
  $md.Add('')
}
$tick = [string][char]0x60
$builtFrom = @($toolsBlock.built_from | Where-Object { $_ } | ForEach-Object { $tick + $_.Substring(0, [math]::Min(12, $_.Length)) + $tick })
$builtText = if ($builtFrom.Count) { $builtFrom -join ', ' } else { 'an unknown commit (no build stamp)' }
$freshText = if ($fresh -and -not $fresh.stale) { ', none stale' } else { '' }
$md.Add("Measured by ``tools/e10-harness.ps1`` from $startedText to $finishedText ($seconds s), checkout at ``$($commitText.Substring(0, [math]::Min(12, $commitText.Length)))``, binaries built from $builtText ($($toolsBlock.source), copied)$freshText; judged $($report.judged_utc).")
$atlasText = if ($settingsBlock.atlas) { [string]$settingsBlock.atlas } else { 'keep' }
$referenceText = if ($settingsBlock.compare_kept) { '; each repacked finest cut compared with the kept build''s from one camera, orbiting lights off (engine-host)' } else { '' }
if ($settingsBlock.build_args) { $atlasText += ' ' + [string]$settingsBlock.build_args }
$md.Add("Captures: $($settingsBlock.width)x$($settingsBlock.height), ``--orbit $($settingsBlock.orbit) --frames $($settingsBlock.frames) --shadows $($settingsBlock.shadows)$(if ($settingsBlock.offscreen) { ' --offscreen' })``$(if ($settingsBlock.offscreen) { ' (offscreen: both cuts through engine-host from that orbit''s camera)' })$(if ($settingsBlock.framing) { " (framing $($settingsBlock.framing))" }), coarse ``--lod $($settingsBlock.coarse_lod_px)`` against finest ``--lod $($settingsBlock.finest_lod_px)``. Containers built with ``--atlas $atlasText``$referenceText.")
$md.Add('')
$md.Add("**Pass rate: $passed of $($rows.Count) ($([math]::Round(100 * $report.totals.pass_rate))%).** Failures by check: import $($byCheck.import), warnings $($byCheck.warnings), atlas island $($byCheck.island), coarse-vs-finest FLIP $($byCheck.flip), LOD collapse $($byCheck.collapse) (an asset can fail more than one).")
$md.Add('')
$md.Add("Thresholds: $MaxWarnings warnings; smallest atlas island >= $MinIslandTexels texel of a 4096 atlas; coarse-vs-finest FLIP mean <= $MaxFlip (the seam fix's numbers put every known-good build below 0.02 and every known-bad one above); the coarse cut draws <= $([math]::Round(100 * $MaxCollapseShare))% of the finest cut's pairs, judged where the finest cut has at least $MinCollapseDensity pairs per 1,000 pixels of the object ($($script:CollapseWhy)).")
$md.Add('')
$md.Add('| asset | triangles | clusters | levels | warn | islands | seam % | smallest island (texels) | median island (texels) | atlas used % | pairs coarse / finest (share; finest per 1,000 object px) | GLB MB | container MB | build ms | PSNR dB | FLIP | object FLIP | coverage % | result |')
$md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
foreach ($r in $rows) {
  $objectFlip = (F $r.flip_object_mean '0.000') + $(if ($r.PSObject.Properties['object_flip_reliable'] -and $r.object_flip_reliable -eq $false) { ' †' } else { '' })
  $pairs = if ($null -ne $r.coarse_visible_pairs -and $null -ne $r.finest_visible_pairs) {
    "$(F $r.coarse_visible_pairs 'N0') / $(F $r.finest_visible_pairs 'N0') ($(F $(if ($null -ne $r.collapse_share) { 100 * $r.collapse_share }) '0')%; $(F $r.finest_pairs_per_kpx '0.0')$(if ($r.checks.collapse -eq 'n/a') { ', too sparse to judge' }))"
  } else { '—' }
  $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16} | {17} | {18} |' -f $r.name, (F $r.triangles 'N0'), (F $r.clusters 'N0'), (F $r.lod_levels '0'),
      (F $r.warnings '0'), (F $r.islands 'N0'), (F $(if ($null -ne $r.seam_fraction) { 100 * $r.seam_fraction }) '0.0'), (F $r.smallest_island_texels_4096 '0.###'),
      (F $r.median_island_texels_4096 'N0'), (F $(if ($null -ne $r.uv_area) { 100 * $r.uv_area }) '0.0'), $pairs, (F $(if ($r.glb_bytes) { $r.glb_bytes / 1MB }) '0.0'),
      (F $(if ($r.container_bytes) { $r.container_bytes / 1MB }) '0.0'), (F $r.build_ms 'N0'), (F $r.psnr '0.0'), (F $r.flip_mean '0.0000'), $objectFlip,
      (F $(if ($null -ne $r.coverage) { 100 * $r.coverage }) '0.0'),
      $(if ($r.status -eq 'pass') { 'pass' } else { "**fail** ($((@($r.checks.Keys | Where-Object { $r.checks[$_] -in 'fail', 'unmeasured' })) -join ', '))" })))
}
$md.Add('')
$thin = @($rows | Where-Object { $_.PSObject.Properties['object_flip_reliable'] -and $_.object_flip_reliable -eq $false })
if ($thin.Count -gt 0) {
  $md.Add(("† **Object FLIP unreliable below {0:0}% coverage** ({1}). It is the whole-frame mean divided by the object's share of the frame, so on a thin object — nearly every pixel of it on the silhouette, where a coarse cut's sub-pixel edge shifts score high, and some of the error falling outside the finest cut's mask altogether — it inflates: a bare tree at 10% coverage scored 0.108 over the object while its whole frame passed at 0.0109. Judge these by the whole-frame FLIP and the heat map." -f (100 * $LowCoverage), (($thin | ForEach-Object { $_.name }) -join ', ')))
  $md.Add('')
}
$repacked = @($rows | Where-Object { $_.PSObject.Properties['atlas'] -and $_.atlas })
$referenced = @($rows | Where-Object { $_.PSObject.Properties['reference'] -and $_.reference })
if ($repacked.Count -gt 0 -or $referenced.Count -gt 0) {
  $md.Add('The atlas step (`--atlas repack`, docs/subsystems/atlas.md): the charts it made (and the proxy charts they came from), the triangles whose new UVs fold against their chart, the atlas, how far the rebaked textures are from their sources on the surface (colour in 8-bit units, the worst image; a normal map in degrees between the object-space normals), and — with `-CompareKept` — the finest cut of this build against the kept build''s, from one camera with the orbiting lights off (both follow each build''s own bounds).')
  $md.Add('')
  $md.Add('| asset | charts (proxy) | folded | atlas | utilization % | colour error mean / p99 | normal error mean / p99 (deg) | PNG MB | repack ms | finest vs kept: FLIP (object) | PSNR dB |')
  $md.Add('|---|---|---|---|---|---|---|---|---|---|---|')
  foreach ($r in $rows) {
    $a = if ($r.PSObject.Properties['atlas']) { $r.atlas } else { $null }
    $ref = if ($r.PSObject.Properties['reference']) { $r.reference } else { $null }
    if (-not $a -and -not $ref) { continue }
    $md.Add(('| {0} | {1} ({13}) | {14} | {2} | {3} | {4} / {5} | {6} / {7} | {8} | {9} | {10} ({11}) | {12} |' -f $r.name,
        (F $(if ($a) { $a.charts }) 'N0'), (F $(if ($a) { $a.resolution }) '0'), (F $(if ($a) { 100 * $a.utilization }) '0.0'),
        (F $(if ($a) { $a.colour_error_mean }) '0.00'), (F $(if ($a) { $a.colour_error_p99 }) '0.0'),
        (F $(if ($a) { $a.normal_error_mean_deg }) '0.00'), (F $(if ($a) { $a.normal_error_p99_deg }) '0.0'),
        (F $(if ($a) { $a.image_bytes / 1MB }) '0.0'), (F $(if ($a) { $a.ms.total }) 'N0'),
        (F $(if ($ref) { $ref.flip_mean }) '0.0000'), (F $(if ($ref) { $ref.flip_object_mean }) '0.000'), (F $(if ($ref) { $ref.psnr }) '0.0'),
        (F $(if ($a) { $a.proxy_charts }) 'N0'), (F $(if ($a) { $a.folded_triangles }) 'N0')))
  }
  $md.Add('')
}
$gen = @($rows | Where-Object { $_.PSObject.Properties['generation'] -and $_.generation })
if ($gen.Count -gt 0) {
  $md.Add('What made them, from each sidecar (`tools/generate.ps1`):')
  $md.Add('')
  $md.Add('| asset | service | image | faces asked | segmenter | smooth iters | weld | texture | unwrap s | bake s | generation s | CPU % (min–max) | GPU % (min–max) |')
  $md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
  foreach ($r in $gen) {
    $gx = $r.generation
    $imageText = if ($gx.PSObject.Properties['image_supplied'] -and $gx.image_supplied) { 'supplied' } else { 'drawn' }
    $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} |' -f $r.name, $r.service, $imageText, (F $gx.face_count 'N0'), $gx.segmenter, $gx.smooth_iters, $gx.weld_distance, $gx.texture,
        (F $gx.unwrap_seconds '0.0'), (F $(if ($gx.PSObject.Properties['bake_seconds']) { $gx.bake_seconds }) '0.0'), (F $gx.wall_seconds '0'),
        $(if ($gx.cpu_pct) { "$($gx.cpu_pct.min)–$($gx.cpu_pct.max)" } else { '—' }), $(if ($gx.gpu_util_pct) { "$($gx.gpu_util_pct.min)–$($gx.gpu_util_pct.max)" } else { '—' })))
  }
  $md.Add('')
}
$md.Add('| column | mean | worst | worst asset | best | best asset |')
$md.Add('|---|---|---|---|---|---|')
foreach ($c in $colStats.Keys) { $s = $colStats[$c]; $md.Add("| $c | $(F $s.mean 'G5') | $(F $s.worst 'G5') | $($s.worst_asset) | $(F $s.best 'G5') | $($s.best_asset) |") }
$md.Add('')
$md.Add("What the coarse-vs-finest FLIP and the LOD collapse follow, as Pearson r over the $($measured.Count) measured assets:")
$md.Add('')
$md.Add('| against | FLIP, whole frame | FLIP, object only | coarse / finest pairs |')
$md.Add('|---|---|---|---|')
foreach ($k in $follows.Keys) { $md.Add("| $k | $(F $follows[$k].flip_mean '0.00') | $(F $follows[$k].flip_object_mean '0.00') | $(F $follows[$k].collapse_share '0.00') |") }
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
if ([int]$ms.samples -eq 0) {
  # -Offscreen captures go through engine-host, whose render.capture samples no machine state.
  $md.Add(("Not sampled: the captures went through engine-host (``-Offscreen``), whose ``render.capture`` reports no machine state, so the build milliseconds are upper bounds of unknown slack. GPU lock at the start: {0}; at the end: {1}. The picture metrics do not depend on load." -f $ms.gpu_lock_at_start, $ms.gpu_lock_at_end))
} else {
  $md.Add(("Sampled by engine-view before and after each of its {0} runs: other processes at {1}–{2}% of the CPU, the GPU {3}–{4}% busy with {5}–{6} MiB of {7} in use; {8} capture runs printed the busy-machine WARNING. GPU lock at the start: {9}; at the end: {10}. The picture metrics do not depend on load; the build milliseconds are upper bounds whenever the CPU figure is high." -f `
      ([int]($ms.samples / 2)), $ms.cpu_others_pct.min, $ms.cpu_others_pct.max, $ms.gpu_util_pct.min, $ms.gpu_util_pct.max, $ms.gpu_memory_used_mib.min, $ms.gpu_memory_used_mib.max, $ms.gpu_memory_total_mib.max, $ms.captures_warned, $ms.gpu_lock_at_start, $ms.gpu_lock_at_end))
}
[IO.File]::WriteAllText((Join-Path $Out 'report.md'), (($md -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))

Write-Log "e10: $passed of $($rows.Count) pass; report at $(Join-Path $Out 'report.md')"
[Console]::Out.WriteLine(([ordered]@{ report = (Join-Path $Out 'report.json'); markdown = (Join-Path $Out 'report.md'); assets = $rows.Count; passed = $passed; pass_rate = $report.totals.pass_rate } | ConvertTo-Json -Compress))
