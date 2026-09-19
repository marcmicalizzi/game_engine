<#
.SYNOPSIS
  Run this engine test bundle and write results.json and results.txt beside it.

.DESCRIPTION
  run-tests.ps1 [-Filter <regex>] [-Jobs <n>] [-DefaultTimeoutSeconds <s>]
                [-SkipAdapters] [-ListOnly]

  Nothing has to be installed on this machine except the Microsoft Visual C++
  2015-2022 x64 redistributable, which this script checks for by name before it
  runs anything - a missing one otherwise turns every executable into the same
  unreadable loader error. A GPU driver is optional: without one every GPU test
  reports that it found no device and passes, which is the intended behaviour
  and not a failure.

  This script is **Windows PowerShell 5.1**. It uses nothing from PowerShell 7,
  no CMake, no ctest and no Visual Studio, because the machines it is for have
  none of them.

  Order of business:

    1. the Visual C++ runtime, by DLL name, with the installer to fetch if it
       is missing;
    2. what this machine is: CPU, RAM, OS build, GPUs and their driver
       versions;
    3. bin\engine-cli gpu.adapters --report adapters.json - the renderer's
       requirements table checked against every device here, with a verdict
       per device saying whether the renderer would run and what it could not
       do. That file is the single most useful thing to send back;
    4. every test in bundle.json, each with its own timeout, each with its own
       stdout and stderr kept under logs\, with the tests that share a resource
       lock never overlapping.

  Results go to results.json (machine-readable, the whole thing) and
  results.txt (readable, one line per test plus the last 50 lines of anything
  that failed).

  Exit code: 0 when nothing failed, 1 when something did, 2 when the bundle
  could not be run at all. A test that skipped because there is no GPU is not a
  failure.

.EXAMPLE
  .\run-tests.ps1
  .\run-tests.ps1 -Filter gfx
  .\run-tests.ps1 -Jobs 4
#>
param(
  [string]$Filter,
  [int]$Jobs = 1,
  [int]$DefaultTimeoutSeconds = 900,
  [switch]$SkipAdapters,
  [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }

function Write-Head([string]$text) {
  Write-Host ''
  Write-Host "== $text" -ForegroundColor Cyan
}

function Write-Utf8([string]$path, [string]$text) {
  $encoding = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($path, $text, $encoding)
}

# The last `count` lines of a log, as **plain strings**.
#
# The cast is not decoration. Every string Get-Content hands back carries PowerShell's note
# properties - PSPath, PSDrive, PSProvider and friends - and PSDrive leads to a provider whose
# Drives collection leads back to the drive. Put one of those lines into an object and hand it to
# ConvertTo-Json -Depth 12 and it walks that cycle: measured here, writing results.json for two
# failed tests took a 6 GB working set and never finished. [string[]] drops the wrapper and keeps
# the text.
function Read-Tail([string]$path, [int]$count) {
  if (-not (Test-Path -LiteralPath $path)) { return @() }
  try {
    $lines = [string[]]@(Get-Content -LiteralPath $path -Tail $count -ErrorAction Stop)
    return , $lines
  }
  catch { return @() }
}

# Windows PowerShell 5.1 hands back a Process from `Start-Process -PassThru` whose ExitCode is
# empty once the process has gone, because nothing kept its native handle open. Reading `.Handle`
# once, while it is still alive, is what makes `HasExited` and `ExitCode` answer afterwards -
# without it every test in this bundle is reported "failed" with a blank exit code, whatever it
# actually did.
function Hold-ProcessHandle($process) {
  if ($null -eq $process) { return }
  try { $null = $process.Handle } catch { }
}

# ---- the bundle --------------------------------------------------------------------------------

$bundlePath = Join-Path $root 'bundle.json'
if (-not (Test-Path -LiteralPath $bundlePath)) {
  Write-Host "no bundle.json beside this script ($bundlePath). Unzip the whole folder and run it from there." -ForegroundColor Red
  exit 2
}
$bundle = Get-Content -LiteralPath $bundlePath -Raw | ConvertFrom-Json

Write-Head "engine test bundle: $($bundle.name)"
Write-Host "commit:   $($bundle.commit_short)  ($($bundle.branch))"
Write-Host "preset:   $($bundle.preset)"
Write-Host "built:    $($bundle.generated_utc) on $($bundle.built_on.machine)"
Write-Host "baseline: $($bundle.cpu_baseline)"
Write-Host "here:     PowerShell $($PSVersionTable.PSVersion)"

# ---- 1. the Visual C++ runtime -----------------------------------------------------------------
#
# Every executable here is MSVC-built and dynamically linked against the C++ runtime. When it is
# absent, Windows fails the load with 0xC0000135 and prints nothing a person can act on, so every
# single test "fails" for the same invisible reason. Name the DLL and the installer instead.

Write-Head 'visual c++ runtime'
$system32 = Join-Path $env:SystemRoot 'System32'
$missingDlls = @()
foreach ($dll in @($bundle.runtime.dlls)) {
  $path = Join-Path $system32 $dll
  if (Test-Path -LiteralPath $path) {
    $version = (Get-Item -LiteralPath $path).VersionInfo.FileVersion
    Write-Host ("  {0,-22} {1}" -f $dll, $version)
  }
  else {
    $missingDlls += $dll
    Write-Host ("  {0,-22} MISSING" -f $dll) -ForegroundColor Red
  }
}
if ($missingDlls.Count -gt 0) {
  Write-Host ''
  Write-Host "This machine is missing: $($missingDlls -join ', ')" -ForegroundColor Red
  Write-Host "Install: $($bundle.runtime.installer)" -ForegroundColor Yellow
  Write-Host "         $($bundle.runtime.url)" -ForegroundColor Yellow
  Write-Host 'Then run this again. Nothing in the bundle can start until it is there.'
  exit 2
}

# ---- 2. what this machine is --------------------------------------------------------------------

function Get-MachineFacts {
  $facts = [ordered]@{
    machine = $env:COMPUTERNAME
    os = ''
    os_build = ''
    architecture = $env:PROCESSOR_ARCHITECTURE
    powershell = $PSVersionTable.PSVersion.ToString()
    cpu = ''
    cpu_cores = 0
    cpu_threads = 0
    cpu_mhz = 0
    ram_mib = 0
    gpus = @()
    free_disk_mib = 0
  }
  try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $facts.os = $os.Caption
    $facts.os_build = "$($os.Version) build $($os.BuildNumber)"
    $facts.ram_mib = [int][math]::Round($os.TotalVisibleMemorySize / 1024)
  }
  catch { }
  try {
    $cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop)[0]
    $facts.cpu = "$($cpu.Name)".Trim()
    $facts.cpu_cores = [int]$cpu.NumberOfCores
    $facts.cpu_threads = [int]$cpu.NumberOfLogicalProcessors
    $facts.cpu_mhz = [int]$cpu.MaxClockSpeed
  }
  catch { }
  try {
    $gpus = @()
    foreach ($v in @(Get-CimInstance Win32_VideoController -ErrorAction Stop)) {
      $gpus += [ordered]@{
        name = "$($v.Name)".Trim()
        driver_version = "$($v.DriverVersion)"
        driver_date = if ($v.DriverDate) { $v.DriverDate.ToString('yyyy-MM-dd') } else { '' }
      }
    }
    $facts.gpus = $gpus
  }
  catch { }
  try {
    $drive = (Get-Item -LiteralPath $root).PSDrive
    if ($drive -and $drive.Free) { $facts.free_disk_mib = [int][math]::Round($drive.Free / 1MB) }
  }
  catch { }
  return $facts
}

Write-Head 'this machine'
$facts = Get-MachineFacts
Write-Host "os:       $($facts.os) ($($facts.os_build), $($facts.architecture))"
Write-Host "cpu:      $($facts.cpu)"
Write-Host "          $($facts.cpu_cores) cores, $($facts.cpu_threads) threads, $($facts.cpu_mhz) MHz"
Write-Host "ram:      $($facts.ram_mib) MiB"
Write-Host "disk:     $($facts.free_disk_mib) MiB free where this bundle sits"
foreach ($g in $facts.gpus) {
  Write-Host "gpu:      $($g.name)  driver $($g.driver_version) ($($g.driver_date))"
}

# The bundle's own paths take over from the ones CMake baked into the binaries: see
# tests/support/test_paths.h. Without this an end-to-end test would look for the executables
# where they were built, which on this machine is a directory that does not exist.
$env:ENGINE_BUNDLE_ROOT = $root.Replace('\', '/')

# ---- 3. the adapter report -----------------------------------------------------------------------

$adapterResult = [ordered]@{ ran = $false; exit_code = $null; file = ''; error = '' }
if (-not $SkipAdapters -and -not $ListOnly) {
  Write-Head 'gpu.adapters'
  $cli = Join-Path $root 'bin\engine-cli.exe'
  if (-not (Test-Path -LiteralPath $cli)) {
    $adapterResult.error = 'bin\engine-cli.exe is not in this bundle'
    Write-Warning $adapterResult.error
  }
  else {
    $reportPath = Join-Path $root 'adapters.json'
    $outPath = Join-Path $root 'adapters.stdout.txt'
    $errPath = Join-Path $root 'adapters.stderr.txt'
    $p = Start-Process -FilePath $cli -ArgumentList @('gpu.adapters', '--report', $reportPath) `
                       -WorkingDirectory $root -NoNewWindow -PassThru `
                       -RedirectStandardOutput $outPath -RedirectStandardError $errPath
    Hold-ProcessHandle $p
    $finished = $p.WaitForExit(120000)
    if (-not $finished) {
      try { & taskkill.exe /PID $p.Id /T /F 2>&1 | Out-Null } catch { }
      $adapterResult.error = 'gpu.adapters did not answer within two minutes'
      Write-Warning $adapterResult.error
    }
    else {
      $adapterResult.ran = $true
      $adapterResult.exit_code = $p.ExitCode
      if (Test-Path -LiteralPath $reportPath) { $adapterResult.file = 'adapters.json' }
      # The interesting lines, so the console says something useful without printing 25 KB.
      if (Test-Path -LiteralPath $reportPath) {
        try {
          $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
          if ($report.result.available -ne $true) {
            Write-Host "no usable Vulkan device: $($report.result.error)" -ForegroundColor Yellow
            Write-Host 'Every GPU test will report that and pass; that is not a failure.'
          }
          else {
            foreach ($a in @($report.result.adapters)) {
              Write-Host "adapter:  $($a.name)  Vulkan $($a.api_version)  driver $($a.driver_name) $($a.driver_info)"
              Write-Host "          hardware tier $($a.tier); renderer verdict: $($a.verdict.tier)"
              foreach ($b in @($a.verdict.blocking)) { Write-Host "  ! $b" -ForegroundColor Red }
              foreach ($d in @($a.verdict.degraded)) { Write-Host "  - $d" -ForegroundColor Yellow }
            }
          }
        }
        catch { Write-Warning "adapters.json is not readable JSON: $($_.Exception.Message)" }
      }
      else {
        $tail = Read-Tail $errPath 10
        $adapterResult.error = ($tail -join ' ')
        Write-Warning "gpu.adapters wrote no report (exit $($p.ExitCode))"
      }
    }
  }
}

# ---- 4. the tests ---------------------------------------------------------------------------------

$selected = @($bundle.tests)
if ($Filter) { $selected = @($selected | Where-Object { $_.name -match $Filter }) }

if ($ListOnly) {
  Write-Head "tests ($($selected.Count))"
  foreach ($t in $selected) {
    $lock = if ($t.resource_lock) { "  lock=$($t.resource_lock)" } else { '' }
    Write-Host ("  {0,-22} {1}{2}" -f $t.name, $t.exe, $lock)
  }
  Write-Head "excluded ($($bundle.excluded_tests.Count))"
  foreach ($e in @($bundle.excluded_tests)) { Write-Host ("  {0,-22} {1}" -f $e.name, $e.reason) }
  exit 0
}

if ($Jobs -lt 1) { $Jobs = 1 }
$logDir = Join-Path $root 'logs'
if (Test-Path -LiteralPath $logDir) { Remove-Item -Recurse -Force -LiteralPath $logDir }
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

Write-Head "running $($selected.Count) tests, $Jobs at a time"

# A test names a resource lock when it needs the machine's one window, one GPU and one set of
# input devices (CTest's RESOURCE_LOCK, carried into bundle.json by tools/package-tests.ps1).
# Two tests holding the same lock never run at once; with -Jobs 1 nothing overlaps at all, which
# is the default because these machines are small.
$pending = New-Object System.Collections.ArrayList
foreach ($t in $selected) { [void]$pending.Add($t) }
$running = @()
$results = @()
$startedAll = Get-Date

function Start-OneTest($test) {
  $exe = Join-Path $root ($test.exe -replace '/', '\')
  $safe = ($test.name -replace '[^A-Za-z0-9._-]', '_')
  $outPath = Join-Path $logDir "$safe.out.txt"
  $errPath = Join-Path $logDir "$safe.err.txt"
  $timeout = [int]$test.timeout_seconds
  if ($timeout -le 0) { $timeout = $DefaultTimeoutSeconds }
  $entry = [ordered]@{
    test = $test
    out = $outPath
    err = $errPath
    started = Get-Date
    timeout = $timeout
    proc = $null
    error = ''
  }
  if (-not (Test-Path -LiteralPath $exe)) {
    $entry.error = "missing from this bundle: $($test.exe)"
    return $entry
  }
  $argList = @($test.args)
  try {
    if ($argList.Count -gt 0) {
      $entry.proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $root `
                                  -NoNewWindow -PassThru `
                                  -RedirectStandardOutput $outPath -RedirectStandardError $errPath
    }
    else {
      $entry.proc = Start-Process -FilePath $exe -WorkingDirectory $root `
                                  -NoNewWindow -PassThru `
                                  -RedirectStandardOutput $outPath -RedirectStandardError $errPath
    }
  }
  catch { $entry.error = "could not start: $($_.Exception.Message)" }
  Hold-ProcessHandle $entry.proc
  return $entry
}

function Complete-OneTest($entry, [string]$state) {
  $test = $entry.test
  $seconds = [math]::Round(((Get-Date) - $entry.started).TotalSeconds, 2)
  $exitCode = $null
  if ($entry.proc) {
    try { $entry.proc.WaitForExit(5000) | Out-Null } catch { }
    try { $exitCode = $entry.proc.ExitCode } catch { }
  }
  $lines = @()
  $lines += Read-Tail $entry.out 400
  $lines += Read-Tail $entry.err 400
  $cases = $null
  $assertions = $null
  $gpuUnavailable = $false
  foreach ($line in $lines) {
    if ($line -match '^\[doctest\]\s+test cases:\s+(\d+)\s*\|\s*(\d+)\s+passed\s*\|\s*(\d+)\s+failed\s*\|\s*(\d+)\s+skipped') {
      $cases = [ordered]@{ total = [int]$Matches[1]; passed = [int]$Matches[2]; failed = [int]$Matches[3]; skipped = [int]$Matches[4] }
    }
    elseif ($line -match '^\[doctest\]\s+assertions:\s+(\d+)\s*\|\s*(\d+)\s+passed\s*\|\s*(\d+)\s+failed') {
      $assertions = [ordered]@{ total = [int]$Matches[1]; passed = [int]$Matches[2]; failed = [int]$Matches[3] }
    }
    # Exactly the sentences a test prints when it stops for want of a device. Loose patterns do
    # not work here: "no display" also appears in engine-input's own usage text, which made a
    # perfectly good run of its end-to-end suite report that this machine had no GPU.
    if ($line -match 'device unavailable:|Vulkan unavailable:|no usable Vulkan device|no Vulkan loader library') {
      $gpuUnavailable = $true
    }
  }

  $note = $entry.error
  if ($state -eq '') {
    if ($null -eq $exitCode) {
      # Hold-ProcessHandle exists to stop this happening; say so rather than call it a failure of
      # the test.
      $state = 'failed'
      if (-not $note) { $note = 'the process exited but Windows PowerShell reported no exit code' }
    }
    elseif ($exitCode -eq 0) { $state = 'passed' }
    elseif ($exitCode -eq -1073741515) { $state = 'missing_runtime' }   # 0xC0000135
    elseif ($exitCode -eq -1073741701) { $state = 'missing_runtime' }   # 0xC000007B
    else { $state = 'failed' }
  }

  $tail = @()
  if ($state -ne 'passed') { $tail = @(Read-Tail $entry.out 50) + @(Read-Tail $entry.err 50) }

  return [ordered]@{
    name = $test.name
    exe = $test.exe
    labels = @($test.labels)
    resource_lock = $test.resource_lock
    status = $state
    exit_code = $exitCode
    seconds = $seconds
    timeout_seconds = $entry.timeout
    gpu_unavailable = $gpuUnavailable
    doctest_cases = $cases
    doctest_assertions = $assertions
    start_error = $note
    log = 'logs/' + [System.IO.Path]::GetFileName($entry.out)
    tail = $tail
  }
}

while ($pending.Count -gt 0 -or $running.Count -gt 0) {
  # Reap anything that finished or outlived its budget.
  $stillRunning = @()
  foreach ($entry in $running) {
    $done = $false
    if (-not $entry.proc) { $done = $true; $state = 'failed' }
    elseif ($entry.proc.HasExited) { $done = $true; $state = '' }
    elseif (((Get-Date) - $entry.started).TotalSeconds -gt $entry.timeout) {
      try { & taskkill.exe /PID $entry.proc.Id /T /F 2>&1 | Out-Null } catch { }
      try { if (-not $entry.proc.HasExited) { $entry.proc.Kill() } } catch { }
      $done = $true
      $state = 'timeout'
    }
    if ($done) {
      $result = Complete-OneTest $entry $state
      $results += $result
      $colour = 'Green'
      if ($result.status -ne 'passed') { $colour = 'Red' }
      $note = ''
      if ($result.gpu_unavailable) { $note = '  (no GPU: parts skipped)' }
      Write-Host ("  {0,-24} {1,-8} {2,7:N2}s{3}" -f $result.name, $result.status, $result.seconds, $note) -ForegroundColor $colour
    }
    else { $stillRunning += $entry }
  }
  $running = $stillRunning

  # Start what can start: a test whose lock nobody is holding.
  while ($pending.Count -gt 0 -and $running.Count -lt $Jobs) {
    $heldLocks = @()
    foreach ($entry in $running) {
      if ($entry.test.resource_lock) { $heldLocks += $entry.test.resource_lock }
    }
    $pick = -1
    for ($i = 0; $i -lt $pending.Count; $i++) {
      $lock = $pending[$i].resource_lock
      if (-not $lock -or ($heldLocks -notcontains $lock)) { $pick = $i; break }
    }
    if ($pick -lt 0) { break }
    $test = $pending[$pick]
    $pending.RemoveAt($pick)
    $running += (Start-OneTest $test)
  }

  if ($running.Count -gt 0) { Start-Sleep -Milliseconds 100 }
}

$totalSeconds = [math]::Round(((Get-Date) - $startedAll).TotalSeconds, 2)

# ---- results --------------------------------------------------------------------------------------

Write-Head 'writing results'

$passed = @($results | Where-Object { $_.status -eq 'passed' }).Count
$failed = @($results | Where-Object { $_.status -ne 'passed' })
$gpuSkipped = @($results | Where-Object { $_.gpu_unavailable }).Count

$summary = [ordered]@{
  schema = 1
  generated_utc = (Get-Date).ToUniversalTime().ToString('o')
  bundle = [ordered]@{
    name = $bundle.name
    commit = $bundle.commit
    commit_short = $bundle.commit_short
    branch = $bundle.branch
    preset = $bundle.preset
    cpu_baseline = $bundle.cpu_baseline
    generated_utc = $bundle.generated_utc
  }
  machine = $facts
  runtime_ok = $true
  adapters = $adapterResult
  jobs = $Jobs
  filter = $Filter
  total_seconds = $totalSeconds
  counts = [ordered]@{
    total = $results.Count
    passed = $passed
    failed = $failed.Count
    gpu_unavailable = $gpuSkipped
    excluded = @($bundle.excluded_tests).Count
  }
  tests = $results
  excluded_tests = @($bundle.excluded_tests)
}
Write-Utf8 (Join-Path $root 'results.json') (($summary | ConvertTo-Json -Depth 12))

$text = New-Object System.Text.StringBuilder
function Add-Line([string]$line) { [void]$text.AppendLine($line) }
Add-Line "engine test bundle: $($bundle.name)"
Add-Line "commit $($bundle.commit_short) ($($bundle.branch)), preset $($bundle.preset), CPU baseline $($bundle.cpu_baseline)"
Add-Line "built $($bundle.generated_utc); run $($summary.generated_utc)"
Add-Line ''
Add-Line 'machine'
Add-Line "  os        $($facts.os) ($($facts.os_build), $($facts.architecture))"
Add-Line "  cpu       $($facts.cpu) - $($facts.cpu_cores) cores, $($facts.cpu_threads) threads, $($facts.cpu_mhz) MHz"
Add-Line "  ram       $($facts.ram_mib) MiB"
Add-Line "  disk      $($facts.free_disk_mib) MiB free"
Add-Line "  shell     PowerShell $($facts.powershell)"
foreach ($g in $facts.gpus) { Add-Line "  gpu       $($g.name) - driver $($g.driver_version) ($($g.driver_date))" }
Add-Line ''
Add-Line 'gpu.adapters'
if ($adapterResult.ran -and $adapterResult.file) { Add-Line "  written to $($adapterResult.file) - send this back" }
elseif ($adapterResult.error) { Add-Line "  not available: $($adapterResult.error)" }
else { Add-Line '  skipped' }
Add-Line ''
Add-Line "tests: $($results.Count) run, $passed passed, $($failed.Count) failed, $gpuSkipped reported no GPU, in $totalSeconds s"
Add-Line ''
foreach ($r in $results) {
  $line = "  {0,-24} {1,-8} {2,7:N2}s" -f $r.name, $r.status, $r.seconds
  if ($r.doctest_assertions) {
    $line += "  {0} assertions, {1} failed" -f $r.doctest_assertions.total, $r.doctest_assertions.failed
  }
  if ($r.gpu_unavailable) { $line += '  (no GPU: parts skipped)' }
  Add-Line $line
}
if ($failed.Count -gt 0) {
  Add-Line ''
  Add-Line '---- failures ----'
  foreach ($r in $failed) {
    Add-Line ''
    Add-Line "$($r.name): $($r.status), exit $($r.exit_code), log $($r.log)"
    if ($r.start_error) { Add-Line "  $($r.start_error)" }
    foreach ($line in $r.tail) { Add-Line "  | $line" }
  }
}
Add-Line ''
Add-Line 'excluded from this bundle on purpose'
foreach ($e in @($bundle.excluded_tests)) { Add-Line "  $($e.name): $($e.reason)" }
Write-Utf8 (Join-Path $root 'results.txt') ($text.ToString())

Write-Head 'result'
Write-Host "$($results.Count) tests, $passed passed, $($failed.Count) failed, $gpuSkipped reported no GPU, in $totalSeconds s"
Write-Host "results.json and results.txt are beside this script."
if ($adapterResult.file) { Write-Host "adapters.json too - that one is the capability report." }
Write-Host 'Send those three files back.'
if ($failed.Count -gt 0) {
  Write-Host ''
  foreach ($r in $failed) { Write-Host "failed: $($r.name) ($($r.status), exit $($r.exit_code)) - see $($r.log)" -ForegroundColor Red }
  exit 1
}
exit 0
