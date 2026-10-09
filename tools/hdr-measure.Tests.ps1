#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/hdr-measure.ps1. Runs under CTest as `tools.hdr_measure`. No GPU, no window.

.DESCRIPTION
  The measurement script's arguments and its results folder, without a device:
  - the refusals (exit 2): an unknown format or view, a paper white of 0, -Width without -Height,
    and a results folder inside the repository;
  - a dry run's plan: every picture of every view, each with `--wait-quiet`, none with
    `--present-max-hz` (the default present ceiling always stands), every window borderless at
    the size asked, the linear radiance offscreen, the erg's own sky in the scene, a camera path a
    view, and a README that tells the owner what to toggle;
  - a run through stand-ins for engine-cli and engine-view (PowerShell scripts that write what the
    real ones would: the probe's report, the EXR, the code histogram and the benchmark), which
    checks the window's size comes from the probe, every histogram becomes a CSV, a format the
    display does not offer (the stand-in exits 3) is recorded and does not fail the run, and the
    README's table carries the distinct codes and the present timings.
  Every case works in its own folder under the system temp directory, a fresh GUID, removed after.

      pwsh tools/hdr-measure.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot 'hdr-measure.ps1'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("engine-hdr-measure-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $root | Out-Null

function Check([bool]$condition, [string]$what) {
  $script:checks++
  if (-not $condition) { $script:failures.Add($what) }
}

function Invoke-Measure([string[]]$arguments) {
  # From inside the case's own folder, so nothing a stand-in writes by a relative name can land in
  # the checkout CTest runs this from (a broken stand-in once wrote its arguments there as files).
  Push-Location $root
  try {
    $output = & pwsh -NoProfile -File $script @arguments *>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $output }
  } finally {
    Pop-Location
  }
}

try {
  # ---- refusals ---------------------------------------------------------------------------------
  $out = Join-Path $root 'refused'
  foreach ($case in @(
      @{ args = @('-Formats', 'hdr12'); says = 'Formats' },
      @{ args = @('-Views', 'noon-north'); says = 'Views' },
      @{ args = @('-PaperWhites', '0'); says = 'PaperWhites' },
      @{ args = @('-Width', '1280'); says = 'go together' },
      @{ args = @('-Seconds', '0'); says = 'Seconds' })) {
    $r = Invoke-Measure (@('-Out', $out, '-DryRun') + $case.args)
    Check ($r.Code -eq 2) "$($case.args -join ' ') exits 2 (got $($r.Code)): $($r.Text)"
    Check ($r.Text -match [regex]::Escape($case.says)) "$($case.args -join ' ') says '$($case.says)': $($r.Text)"
  }
  $inside = Join-Path $repo 'build/hdr-results'
  $r = Invoke-Measure @('-Out', $inside, '-DryRun')
  Check ($r.Code -eq 2 -and $r.Text -match 'inside the repository') "a results folder inside the repository is refused: $($r.Text)"
  Check (-not (Test-Path -LiteralPath $inside)) 'the refused folder was not created'

  # ---- a dry run's plan -------------------------------------------------------------------------
  $out = Join-Path $root 'dry'
  $r = Invoke-Measure @('-Out', $out, '-DryRun', '-Width', '1280', '-Height', '720', '-WaitQuiet', '60')
  Check ($r.Code -eq 0) "a dry run exits 0: $($r.Text)"
  $plan = Get-Content -LiteralPath (Join-Path $out 'plan.json') -Raw | ConvertFrom-Json
  $views = @('ramp', 'dusk-west', 'dusk-east', 'dawn-east', 'dawn-west', 'night-east', 'night-west')
  # Per view: sdr10 dither on and off, hdr10 at 100, 200, 300 and Windows' white, scrgb, linear.
  Check (@($plan.runs).Count -eq $views.Count * 8) "7 views x 8 pictures planned (got $(@($plan.runs).Count))"
  Check ($plan.dry_run -eq $true) 'the plan says it is a dry run'
  foreach ($run in $plan.runs) {
    $a = @($run.args)
    $what = "$($run.view)/$($run.name)"
    Check ($a -contains '--wait-quiet' -and $a[[array]::IndexOf($a, '--wait-quiet') + 1] -eq '60') "$what waits for a quiet machine"
    Check (-not ($a -contains '--present-max-hz')) "$what never lifts the present ceiling"
    Check ($a -contains '--capture' -and $a -contains '--benchmark') "$what captures and times"
    Check ($a[[array]::IndexOf($a, '--width') + 1] -eq '1280' -and $a[[array]::IndexOf($a, '--height') + 1] -eq '720') "$what is 1280 x 720"
    if ($run.name -eq 'linear') {
      Check ($a -contains '--offscreen' -and -not ($a -contains '--windowed')) "$what is offscreen"
      Check ($run.capture -like '*.exr') "$what is an EXR"
    } else {
      Check ($a -contains '--windowed' -and $a -contains '--borderless') "$what is a borderless window"
    }
    if ($run.name -like 'sdr10-*') {
      Check ($run.capture -like '*.png' -and $a -contains 'light') "$what writes a PNG and its light"
    }
    if ($run.view -eq 'ramp') { Check ($a -contains 'ramp') "$what draws the ramp" }
    if ($run.view -like 'dusk-*') { Check ($a[[array]::IndexOf($a, '--time-of-day') + 1] -eq '18.75') "$what is at 18:45" }
    Check ((Split-Path (Split-Path $run.capture -Parent) -Leaf) -eq $run.view) "$what is captured in its view's folder"
  }
  $names = @($plan.runs | Where-Object { $_.view -eq 'ramp' } | ForEach-Object { $_.name })
  foreach ($n in @('sdr10-dither-on', 'sdr10-dither-off', 'hdr10-pw100', 'hdr10-pw200', 'hdr10-pw300', 'hdr10-pw-windows', 'scrgb', 'linear')) {
    Check ($names -contains $n) "the ramp has $n"
  }
  $scene = Get-Content -LiteralPath (Join-Path $out 'scenes/sky.json') -Raw | ConvertFrom-Json
  $erg = Get-Content -LiteralPath (Join-Path $repo 'content/test-scenes/desert-erg/scene.json') -Raw | ConvertFrom-Json
  Check ($scene.sky.latitude_deg -eq $erg.sky.latitude_deg -and $scene.sky.moon_age_days -eq $erg.sky.moon_age_days) "the scene's sky is the erg's"
  Check ($scene.terrain.size -eq 65) 'the scene is the small terrain'
  foreach ($v in $views) { Check (Test-Path -LiteralPath (Join-Path $out "paths/$v.json")) "a camera path for $v" }
  $path = Get-Content -LiteralPath (Join-Path $out 'paths/dusk-west.json') -Raw | ConvertFrom-Json
  Check ($path.keys[0].target[0] -lt 0 -and @($path.keys).Count -eq 2) 'west looks along -x, a still camera of two keys'
  $readme = Get-Content -LiteralPath (Join-Path $out 'README.md') -Raw
  Check ($readme -match 'Use HDR' -and $readme -match '10 bpc') 'the README says what the owner toggles'
  Check ($readme -match 'dry run') 'the README says nothing was drawn'
  Check (-not ($readme -match 'present-max-hz')) 'no command in the README lifts the present ceiling'

  # A subset of formats and paper whites is what is planned.
  $out = Join-Path $root 'subset'
  $r = Invoke-Measure @('-Out', $out, '-DryRun', '-Formats', 'hdr10', '-PaperWhites', '150', '-Views', 'night-east')
  $plan = Get-Content -LiteralPath (Join-Path $out 'plan.json') -Raw | ConvertFrom-Json
  $names = @($plan.runs | ForEach-Object { $_.name })
  Check (($names -join ',') -eq 'hdr10-pw150,hdr10-pw-windows,linear') "a subset plans only it (got $($names -join ','))"

  # ---- a run through stand-ins ------------------------------------------------------------------
  $bin = Join-Path $root 'bin'
  New-Item -ItemType Directory -Force -Path $bin | Out-Null
  $cli = Join-Path $bin 'engine-cli.ps1'
  @'
[string[]]$a = @($args | ForEach-Object { "$_" })
$report = $a[[array]::IndexOf($a, '--report') + 1]
$result = @{ available = $true; outputs = @(
    @{ name = '\\.\DISPLAY1'; adapter = 'stand-in'; desktop = @(0, 0, 800, 450); bits_per_color = 10
       color_space = 'rgb_full_g2084_none_p2020'; hdr = $true; min_luminance = 0.0001; max_luminance = 1015
       max_full_frame_luminance = 658; sdr_white_nits = 280; surfaces = @() }) }
@{ tool = 'engine-cli'; method = 'gpu.displays'; result = $result } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report
exit 0
'@ | Set-Content -LiteralPath $cli -Encoding utf8
  $view = Join-Path $bin 'engine-view.ps1'
  @'
[string[]]$a = @($args | ForEach-Object { "$_" })
[string]$capture = $a[[array]::IndexOf($a, '--capture') + 1]
[string]$bench = $a[[array]::IndexOf($a, '--benchmark') + 1]
[string]$format = $a[[array]::IndexOf($a, '--present-format') + 1]
if ($format -eq $env:HDR_MEASURE_TEST_UNOFFERED) { Write-Output "not offered"; exit 3 }
$stem = $capture -replace '\.(png|exr)$', ''
if ($capture -like '*.png') { Set-Content -LiteralPath $capture -Value 'png' }
Set-Content -LiteralPath "$stem.exr" -Value 'exr'
$counts = @(@(5, 0, 3), @(0, 8, 0), @(1, 1, 6))
@{ format = 'engine.renderer.codes.v1'; domain = 'pq'; bits = 10; distinct = @(2, 1, 3); min = @(0, 1, 0); max = @(2, 1, 2); counts = $counts } |
  ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$stem.codes.json"
Set-Content -LiteralPath $bench -Value @('{"frame":0}', '{"presentation":{"present_ms":{"p50":1.25,"p95":2.5}}}')
Write-Output ($args -join ' ')
exit 0
'@ | Set-Content -LiteralPath $view -Encoding utf8
  $out = Join-Path $root 'stand-in'
  $env:HDR_MEASURE_TEST_UNOFFERED = 'scrgb'
  try {
    $r = Invoke-Measure @('-Out', $out, '-NoLock', '-EngineView', $view, '-EngineCli', $cli,
      '-Views', 'ramp,dusk-west', '-WaitQuiet', '0')
  } finally {
    Remove-Item Env:HDR_MEASURE_TEST_UNOFFERED -ErrorAction SilentlyContinue
  }
  Check ($r.Code -eq 0) "a run through the stand-ins exits 0, an unoffered format and all: $($r.Text)"
  if ($r.Code -ne 0) { throw "the stand-in run failed: $($r.Text)" }
  $plan = Get-Content -LiteralPath (Join-Path $out 'plan.json') -Raw | ConvertFrom-Json
  Check ($plan.width -eq 800 -and $plan.height -eq 450 -and $plan.hdr_on -eq $true -and $plan.peak_nits -eq 1015) 'the window and the peak are the probe''s'
  Check (Test-Path -LiteralPath (Join-Path $out 'displays.json')) 'the probe''s report is kept'
  $csv = Get-Content -LiteralPath (Join-Path $out 'ramp/hdr10-pw200.histogram.csv')
  Check ($csv[0] -eq 'code,r,g,b' -and $csv.Count -eq 4 -and $csv[1] -eq '0,5,0,1') "a histogram becomes a CSV (got $($csv -join ' | '))"
  Check (Test-Path -LiteralPath (Join-Path $out 'dusk-west/sdr10-dither-on.png')) 'an SDR picture has its PNG'
  Check (Test-Path -LiteralPath (Join-Path $out 'dusk-west/sdr10-dither-on.histogram.csv')) 'and its light''s histogram'
  $readme = Get-Content -LiteralPath (Join-Path $out 'README.md') -Raw
  Check ($readme -match '\| ramp \| \[scrgb\.exr\]\(ramp/scrgb\.exr\) \| not offered \|') "the unoffered scRGB is recorded: $readme"
  Check ($readme -match '2/1/3 \(pq, 10 bits\)') 'the README carries the distinct codes'
  Check ($readme -match 'present_ms 1\.25/2\.5') 'the README carries the present timings'
  Check ($readme -match 'HDR is \*\*on\*\*') 'the README says what the probe found'
} finally {
  if ($KeepTemp) { Write-Host "kept $root" } else { Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue }
}

if ($failures.Count -gt 0) {
  foreach ($f in $failures) { Write-Host "FAIL: $f" -ForegroundColor Red }
  Write-Host "hdr-measure.Tests: $($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "hdr-measure.Tests: all $checks checks passed"
exit 0
