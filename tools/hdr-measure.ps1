#!/usr/bin/env pwsh
<#
.SYNOPSIS
  E39's measurements on this machine, into one results folder the owner reads while he looks at
  the panels (docs/experiments/hdr-output-proposal.md, "What the owner measures"; roadmap R80).

.DESCRIPTION
  tools/hdr-measure.ps1 [-Out <dir>] [-Preset msvc-release] [-Bin <dir>]
                        [-Formats sdr10,hdr10,scrgb] [-PaperWhites 100,200,300]
                        [-Views ramp,dusk-west,dusk-east,dawn-east,dawn-west,night-east,night-west]
                        [-Width <px> -Height <px>] [-Seconds 4] [-WaitQuiet 300]
                        [-NoLock] [-DryRun] [-EngineView <exe>] [-EngineCli <exe>]

  Under `tools/gpu-lock.ps1 run` (one lock for the whole measurement, so nothing else's work lands
  between two pictures the owner is comparing; -NoLock leaves it out), in this order:

  1. **The probe** (measurement 1): `engine-cli gpu.displays --report displays.json` — every output
     Windows reports, whether "Use HDR" is on, its luminances and SDR white level, and the formats
     and colour spaces a window on it is offered. The output at the desktop's origin sets the
     window's size (borderless, covering it) and its reported peak is the HDR runs' peak.
  2. **The pictures**, for each view: the ramp (`--view ramp`: black to white, the darkest eighth,
     four stops past white, through the output encode) and the erg's sky over a small terrain at
     the banding test's hours (docs/experiments/sky-banding-2026-10-04.md: dusk 18:45, dawn 05:30,
     the moonlit night 20:00), looking west and east 24 degrees up — through
       - `sdr10` with the dither on and off (measurement 2), a PNG and an EXR of each;
       - `hdr10` at each paper white and at Windows' SDR white level, the reported peak in all of
         them (measurement 3), an EXR of each;
       - `scrgb` (measurement 4), an EXR;
       - and offscreen, the radiance before any curve (`--present-format linear`), an EXR: what
         every curve was given.
     Each window is borderless over the display, flies a still camera for -Seconds with
     `--benchmark` (present timings per presented frame) and `--wait-quiet`, and is captured on its
     last frame. Every EXR comes with `<stem>.codes.json`, the histogram of the codes the picture
     was stored in (PQ codes for hdr10 and scrgb), which this script also writes as
     `<stem>.histogram.csv`.
  3. **README.md**: what was found, what to look at on the panels for each picture, a table of
     every capture (distinct codes a channel, their range) and its present timings, and the
     commands that made them.

  It never changes a display setting: Windows' "Use HDR" and the NVIDIA control panel's output
  colour depth are the owner's, and between the two runs E39 needs (HDR off, then on) he toggles
  them himself; the script reads what DXGI reports and says what it found. It never opens a window
  without the default present ceiling (`view.present.max_hz`): `--present-max-hz` is not a flag it
  passes. A format the display does not offer (HDR10 with HDR off, say) is recorded as not offered
  and the run goes on.

  -DryRun writes the scene, the camera paths, the plan (`plan.json`) and a README of what would run,
  and runs nothing: what tools/hdr-measure.Tests.ps1 checks without a GPU. -EngineView and
  -EngineCli name other executables (the tests' stand-ins). The results folder is refused inside
  the repository: captures are not committed (AGENTS.md).

  Exit codes: 0 every run answered (offered or not); 1 a run failed or wrote nothing; 2 usage.
#>
[CmdletBinding()]
param(
  [string]$Out = '',
  [string]$Preset = 'msvc-release',
  [string]$Bin = '',
  [string[]]$Formats = @('sdr10', 'hdr10', 'scrgb'),
  [double[]]$PaperWhites = @(100, 200, 300),
  [string[]]$Views = @('ramp', 'dusk-west', 'dusk-east', 'dawn-east', 'dawn-west', 'night-east', 'night-west'),
  [int]$Width = 0,
  [int]$Height = 0,
  [double]$Seconds = 4,
  [int]$WaitQuiet = 300,
  [switch]$NoLock,
  [switch]$DryRun,
  [string]$EngineView = '',
  [string]$EngineCli = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Fail-Usage([string]$message) {
  [Console]::Error.WriteLine("hdr-measure: $message")
  exit 2
}

# ---- the arguments ------------------------------------------------------------------------------
# A comma list arrives as one string from `pwsh -File`; split it so both spellings work.
$Formats = @($Formats | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Views = @($Views | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$knownFormats = @('sdr10', 'hdr10', 'scrgb')
foreach ($f in $Formats) {
  if ($knownFormats -notcontains $f) { Fail-Usage "-Formats takes $($knownFormats -join ', '), not '$f'" }
}
# The views: the ramp, and the banding test's three hours looking west or east.
$hours = @{ dusk = 18.75; dawn = 5.5; night = 20.0 }
foreach ($v in $Views) {
  if ($v -ne 'ramp' -and $v -notmatch '^(dusk|dawn|night)-(west|east)$') {
    Fail-Usage "-Views takes ramp or <dusk|dawn|night>-<west|east>, not '$v'"
  }
}
if ($Views.Count -eq 0) { Fail-Usage '-Views names nothing to draw' }
foreach ($p in $PaperWhites) {
  if (-not ($p -gt 0 -and $p -le 10000)) { Fail-Usage "-PaperWhites are nits in (0, 10000], not $p" }
}
if ($Seconds -le 0 -or $Seconds -gt 60) { Fail-Usage "-Seconds is in (0, 60], not $Seconds" }
if ($WaitQuiet -lt 0) { Fail-Usage "-WaitQuiet is seconds, not $WaitQuiet" }
if (($Width -gt 0) -xor ($Height -gt 0)) { Fail-Usage '-Width and -Height go together' }
if (-not $Out) {
  $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
  $Out = Join-Path (Split-Path $repo -Parent) "game_engine_local\e39\$stamp"
}
$Out = [System.IO.Path]::GetFullPath($Out)
$repoFull = [System.IO.Path]::GetFullPath($repo).TrimEnd('\', '/')
if ($Out.StartsWith($repoFull + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    $Out.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase)) {
  Fail-Usage "the results folder $Out is inside the repository; captures are never committed (AGENTS.md)"
}
if (-not $Bin) { $Bin = Join-Path $repo "build/$Preset/bin" }
$exe = if ($IsWindows -or $env:OS -eq 'Windows_NT') { '.exe' } else { '' }
if (-not $EngineView) { $EngineView = Join-Path $Bin "engine-view$exe" }
if (-not $EngineCli) { $EngineCli = Join-Path $Bin "engine-cli$exe" }
if (-not $DryRun) {
  foreach ($e in @($EngineView, $EngineCli)) {
    if (-not (Test-Path -LiteralPath $e)) { Fail-Usage "$e does not exist; build -Preset $Preset first" }
  }
}

# ---- one GPU lock for the whole measurement -----------------------------------------------------
if (-not $DryRun -and -not $NoLock -and -not $env:ENGINE_GPU_LOCK_HOLDER) {
  $self = @('-NoProfile', '-File', $PSCommandPath) + ($PSBoundParameters.GetEnumerator() | ForEach-Object {
      if ($_.Value -is [switch]) { if ($_.Value) { "-$($_.Key)" } }
      elseif ($_.Value -is [array]) { "-$($_.Key)"; ($_.Value -join ',') }
      else { "-$($_.Key)"; "$($_.Value)" }
    })
  if (-not $PSBoundParameters.ContainsKey('Out')) { $self += @('-Out', $Out) }
  $quoted = ($self | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '
  & (Join-Path $PSScriptRoot 'gpu-lock.ps1') run -Purpose 'E39 HDR measurement (tools/hdr-measure.ps1)' `
    -Exec "pwsh $quoted"
  exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
foreach ($d in @('scenes', 'paths')) { New-Item -ItemType Directory -Force -Path (Join-Path $Out $d) | Out-Null }

# ---- 1. the probe -------------------------------------------------------------------------------
$probe = $null
$displaysFile = Join-Path $Out 'displays.json'
if (-not $DryRun) {
  & $EngineCli gpu.displays --report $displaysFile | Out-Null
  if (Test-Path -LiteralPath $displaysFile) {
    $probe = (Get-Content -LiteralPath $displaysFile -Raw | ConvertFrom-Json).result
  }
}
$primary = $null
if ($probe -and $probe.outputs) {
  $primary = @($probe.outputs | Where-Object { $_.desktop -and $_.desktop[0] -eq 0 -and $_.desktop[1] -eq 0 })[0]
  if (-not $primary) { $primary = @($probe.outputs)[0] }
}
if ($Width -le 0) {
  if ($primary -and $primary.desktop.Count -eq 4) {
    $Width = [int]($primary.desktop[2] - $primary.desktop[0])
    $Height = [int]($primary.desktop[3] - $primary.desktop[1])
  } else {
    $Width = 1920; $Height = 1080
  }
}
$hdrOn = [bool]($primary -and $primary.hdr)
$peak = if ($primary -and $primary.max_luminance -gt 0) { [double]$primary.max_luminance } else { 0 }

# ---- the scene and the camera paths -------------------------------------------------------------
# The erg's own sky (its calendar, its moon, its air) over the banding test's small terrain, so a
# picture is the erg's sky in seconds rather than the erg's dunes in minutes.
$erg = Get-Content -LiteralPath (Join-Path $repo 'content/test-scenes/desert-erg/scene.json') -Raw | ConvertFrom-Json
$scene = [ordered]@{
  format  = 'engine.scene.v1'
  name    = 'e39-sky'
  sky     = $erg.sky
  terrain = [ordered]@{ size = 65; extent = 64; seed = 5 }
}
$sceneFile = Join-Path $Out 'scenes/sky.json'
$scene | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $sceneFile -Encoding utf8

function Write-Path([string]$name, [double]$azimuthX) {
  # A still camera on the ground looking 24 degrees up, west (-x) or east (+x): two equal keys
  # -Seconds apart, so the window flies it in real time and the benchmark has frames to time.
  $up = [math]::Tan(24 * [math]::PI / 180) * 100
  $key = { param($t) [ordered]@{ time = $t; position = @(0, 1.65, 0); target = @((100 * $azimuthX), (1.65 + $up), -6) } }
  $path = [ordered]@{
    format        = 'engine.camera-path.v1'
    name          = "e39-$name"
    interpolation = 'Smooth'
    fps           = 60
    znear         = 0.1
    keys          = @((& $key 0), (& $key $Seconds))
  }
  $file = Join-Path $Out "paths/$name.json"
  $path | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $file -Encoding utf8
  return $file
}

# ---- 2. the plan --------------------------------------------------------------------------------
$runs = New-Object System.Collections.Generic.List[object]
foreach ($view in $Views) {
  $hour = 12.0
  $direction = 'west'
  if ($view -ne 'ramp') {
    $parts = $view -split '-'
    $hour = $hours[$parts[0]]
    $direction = $parts[1]
  }
  $pathFile = Write-Path $view ($(if ($direction -eq 'west') { -1 } else { 1 }))
  $dir = Join-Path $Out $view
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $common = @('--scene', $sceneFile, '--camera-path', $pathFile, '--time-of-day', "$hour",
    '--wait-quiet', "$WaitQuiet")
  if ($view -eq 'ramp') { $common += @('--view', 'ramp') }
  $window = @('--windowed', '--borderless', '--width', "$Width", '--height', "$Height")
  $configs = New-Object System.Collections.Generic.List[object]
  if ($Formats -contains 'sdr10') {
    foreach ($d in @('on', 'off')) {
      $configs.Add([ordered]@{ name = "sdr10-dither-$d"; capture = "sdr10-dither-$d.png"
          args = @('--present-format', 'sdr10', '--dither', $d, '--capture-channels', 'light') + $window })
    }
  }
  if ($Formats -contains 'hdr10') {
    foreach ($p in $PaperWhites) {
      $configs.Add([ordered]@{ name = "hdr10-pw$p"; capture = "hdr10-pw$p.exr"
          args = @('--present-format', 'hdr10', '--paper-white-nits', "$p") + $window })
    }
    $configs.Add([ordered]@{ name = 'hdr10-pw-windows'; capture = 'hdr10-pw-windows.exr'
        args = @('--present-format', 'hdr10') + $window })
  }
  if ($Formats -contains 'scrgb') {
    $configs.Add([ordered]@{ name = 'scrgb'; capture = 'scrgb.exr'; args = @('--present-format', 'scrgb') + $window })
  }
  # The radiance before any curve, offscreen at the window's size: what every picture above began as.
  $configs.Add([ordered]@{ name = 'linear'; capture = 'linear.exr'
      args = @('--present-format', 'linear', '--offscreen', '--width', "$Width", '--height', "$Height") })
  foreach ($c in $configs) {
    $capture = Join-Path $dir $c.capture
    $bench = Join-Path $dir "$($c.name).jsonl"
    $runs.Add([ordered]@{
        view    = $view
        hour    = $hour
        name    = $c.name
        capture = $capture
        bench   = $bench
        args    = @($common + $c.args + @('--capture', $capture, '--benchmark', $bench))
      })
  }
}
$plan = [ordered]@{
  format    = 'engine.hdr-measure.v1'
  out       = $Out
  dry_run   = [bool]$DryRun
  width     = $Width
  height    = $Height
  hdr_on    = $hdrOn
  peak_nits = $peak
  runs      = $runs
}
$plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Out 'plan.json') -Encoding utf8

# ---- running them -------------------------------------------------------------------------------
function Write-HistogramCsv([string]$codesFile) {
  # `<stem>.codes.json` (renderer/capture.h, CodeHistogram) as `<stem>.histogram.csv`: one row a code.
  $codes = Get-Content -LiteralPath $codesFile -Raw | ConvertFrom-Json
  $csv = [System.Text.StringBuilder]::new()
  [void]$csv.AppendLine("code,r,g,b")
  $levels = @($codes.counts[0]).Count
  for ($k = 0; $k -lt $levels; $k++) {
    [void]$csv.AppendLine("$k,$($codes.counts[0][$k]),$($codes.counts[1][$k]),$($codes.counts[2][$k])")
  }
  $target = $codesFile -replace '\.codes\.json$', '.histogram.csv'
  Set-Content -LiteralPath $target -Value $csv.ToString() -Encoding utf8 -NoNewline
  return $codes
}

function Get-PresentSummary([string]$benchFile) {
  # The benchmark's last line is its summary; its `presentation` block has the percentiles of every
  # wait and display time. Said in one line: each timing's p50 and p95.
  if (-not (Test-Path -LiteralPath $benchFile)) { return '' }
  $last = Get-Content -LiteralPath $benchFile | Where-Object { $_.Trim() } | Select-Object -Last 1
  try { $summary = $last | ConvertFrom-Json } catch { return '' }
  $p = $summary.presentation
  if (-not $p) { return '' }
  $parts = @()
  foreach ($prop in $p.PSObject.Properties) {
    $v = $prop.Value
    if ($v -is [System.Management.Automation.PSCustomObject] -and $null -ne $v.p50) {
      $parts += "$($prop.Name) $([math]::Round([double]$v.p50, 2))/$([math]::Round([double]$v.p95, 2))"
    }
  }
  return ($parts -join ', ')
}

$results = New-Object System.Collections.Generic.List[object]
$failed = 0
foreach ($run in $runs) {
  $result = [ordered]@{ run = $run; status = 'planned'; exit = $null; codes = $null; present = '' }
  if (-not $DryRun) {
    Write-Host "hdr-measure: $($run.view) $($run.name)"
    $runArgs = @($run.args)
    & $EngineView @runArgs *> ($run.capture -replace '\.(png|exr)$', '.log')
    $code = $LASTEXITCODE
    $result.exit = $code
    $exrFile = $run.capture -replace '\.png$', '.exr'
    $codesFile = $exrFile -replace '\.exr$', '.codes.json'
    if ($code -eq 0 -and (Test-Path -LiteralPath $exrFile) -and (Test-Path -LiteralPath $codesFile)) {
      $result.status = 'captured'
      $result.codes = Write-HistogramCsv $codesFile
      $result.present = Get-PresentSummary $run.bench
    } elseif ($code -eq 3) {
      $result.status = 'not offered'  # the surface does not offer the format, or no device
    } elseif ($code -eq 4) {
      $result.status = 'machine busy'
      $failed++
    } else {
      $result.status = "failed (exit $code)"
      $failed++
    }
  }
  $results.Add($result)
}

# ---- 3. README.md -------------------------------------------------------------------------------
$md = [System.Text.StringBuilder]::new()
function Line([string]$text = '') { [void]$md.AppendLine($text) }
Line "# E39: HDR output, measured $((Get-Date).ToString('yyyy-MM-dd HH:mm'))"
Line
Line "What this folder is: the measurements of docs/experiments/hdr-output-proposal.md (E39), made by ``tools/hdr-measure.ps1``$(if ($DryRun) { ' **as a dry run: nothing was drawn**' })."
Line
Line '## Before you look: what you set, and what the script found'
Line
Line 'The script changes no display setting. Between the two runs E39 needs you set them yourself:'
Line
Line '- **Run 1, HDR off**: Windows Settings > System > Display > "Use HDR" off for the Surround display, and the NVIDIA control panel''s output colour depth at 10 bpc. The `sdr10` pictures are measurement 2; `hdr10` and `scrgb` will say "not offered" if the driver offers neither without HDR.'
Line '- **Run 2, HDR on**: "Use HDR" on (leave the SDR content brightness where you keep it: it is the `hdr10-pw-windows` paper white). Every picture runs.'
Line
if ($probe) {
  Line "The probe (``displays.json``): HDR is **$(if ($hdrOn) { 'on' } else { 'off' })** on the output at the desktop's origin."
  Line
  Line '| output | bits | colour space | HDR | min / peak / full frame (nits) | SDR white | offered past sRGB |'
  Line '|---|---|---|---|---|---|---|'
  foreach ($o in $probe.outputs) {
    $offers = @($o.surfaces | ForEach-Object { $_.formats } | Where-Object { $_ -and $_.color_space -ne 'srgb_nonlinear' } |
        ForEach-Object { "$($_.format) $($_.color_space)" }) -join ', '
    Line "| $($o.name) $($o.desktop -join ',') | $($o.bits_per_color) | $($o.color_space) | $($o.hdr) | $($o.min_luminance) / $($o.max_luminance) / $($o.max_full_frame_luminance) | $($o.sdr_white_nits) | $offers |"
  }
} else {
  Line 'The probe did not run (a dry run, or `engine-cli gpu.displays` wrote nothing).'
}
Line
Line "Every window was ${Width} x ${Height}, borderless over the display, with the default present ceiling, for $Seconds s a picture; the HDR runs' peak is the display's reported one$(if ($peak -gt 0) { " ($peak nits)" })."
Line
Line '## What to look at'
Line
Line '- **ramp**: the top half climbs one code every few pixels from black to white; the third quarter is its darkest eighth stretched across; the bottom quarter is four stops past white. In `sdr10-dither-off` look for steps across the top and the dark band; in `sdr10-dither-on` for whether they are gone and the grain is visible. In `hdr10-pw*` the bottom band should roll off towards the peak instead of clipping; the dark band is where PQ''s steps are largest.'
Line '- **dusk / dawn / night, west and east**: the banding test''s slowest skies. Bands across the sky away from a low sun and under the moon; the glow round the sun at dusk and dawn. HDR should show the same sky below paper white and more of the glow above it.'
Line '- Between `hdr10-pw100`, `-pw200`, `-pw300` and `-pw-windows` only paper white moves: everything below it should brighten together, highlights should not.'
Line '- `scrgb` against `hdr10-pw-windows`: the same light; any difference is the compositor''s conversion.'
Line '- `linear.exr` is the radiance before any curve, 1.0 a white surface under the metered light; every other EXR is that picture''s light with 1.0 at its white (paper white for HDR, whose nits are the file''s `whiteLuminance`).'
Line
Line '## Captures'
Line
Line '| view | picture | status | distinct codes r/g/b (domain) | code range r | present p50/p95 (ms) |'
Line '|---|---|---|---|---|---|'
foreach ($r in $results) {
  $run = $r.run
  $codes = if ($r.codes) { "$($r.codes.distinct -join '/') ($($r.codes.domain), $($r.codes.bits) bits)" } else { '' }
  $range = if ($r.codes) { "$($r.codes.min[0])..$($r.codes.max[0])" } else { '' }
  $file = Split-Path $run.capture -Leaf
  Line "| $($run.view) | [$file]($($run.view)/$file) | $($r.status) | $codes | $range | $($r.present) |"
}
Line
Line 'Beside each capture: `<stem>.codes.json` (the histogram), `<stem>.histogram.csv` (the same, one row a code), `<name>.jsonl` (the benchmark: a record per presented frame, then the summary) and `<stem>.log` (engine-view''s output).'
Line
Line '## The commands'
Line
Line '```powershell'
Line "& `"$EngineCli`" gpu.displays --report `"$displaysFile`""
foreach ($run in $runs) {
  Line ("& `"$EngineView`" " + (($run.args | ForEach-Object { if ($_ -match '[\s"]') { "`"$_`"" } else { $_ } }) -join ' '))
}
Line '```'
Set-Content -LiteralPath (Join-Path $Out 'README.md') -Value $md.ToString() -Encoding utf8

Write-Host "hdr-measure: $($runs.Count) picture(s)$(if ($DryRun) { ' planned' }) in $Out"
if ($failed -gt 0) {
  Write-Host "hdr-measure: $failed run(s) failed; README.md says which" -ForegroundColor Red
  exit 1
}
exit 0
