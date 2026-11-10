#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Nothing this tree builds opens a listening socket on a non-loopback address.

.DESCRIPTION
  On 2026-09-18 pending Windows Firewall prompts took the development machine's GPU down for
  hours: each prompt is a `PickerHost.exe FirewallNotificationDialogServer` process holding a GPU
  context, thirty-five of them took the box from 49 GPU client processes to 77, and somewhere
  between those two numbers the NVIDIA driver stopped surviving `vkCreateDevice`. The listener was
  Tracy's, which binds every interface by default; `ENGINE_TRACY` now sets `TRACY_ONLY_LOCALHOST`
  and `TRACY_NO_BROADCAST` (docs/subsystems/profiling.md). This script is the check that no second
  one appears. It runs at the merge, once, after the suites (docs/ci/self-hosted-runners.md,
  "tools/ci/check-no-listeners.ps1", has the gate's command line and why it is not a CTest test);
  its rules are tools/lib/NoListeners.psm1's and are tested there with no listener at all.

  Windows raises the prompt **once per executable path**, and a path it has already decided about
  is silent forever after — which is exactly what makes the problem invisible on a developer's
  machine and fatal on a fresh one. So the check copies every executable the build produced into a
  directory with a random name, making every path new to the firewall, runs each one once with
  harmless arguments, and counts the prompt processes before and after. The count must not grow.

  What it leaves behind is nothing, and it is built so that holds on a shared machine:
    - every copy runs under an error mode that shows no box (a missing DLL, a crash): a binary that
      cannot start exits with its status instead of waiting for a click (Set-QuietErrorMode);
    - the copies find their DLLs through PATH, from the build directory they were copied from, so
      they start rather than fail in the loader;
    - nothing opens a window: engine-view and engine-editor run `--offscreen`, a bench `--list`s;
    - a device a copy opens takes the machine-wide GPU lock as a test's does
      (ENGINE_GPU_LOCK_ON_DEVICE=1, unless the caller set it);
    - after each binary, and again at the end, every process whose image lies under the run's own
      directory is ended — those are this run's copies and their children, whatever their pids;
    - a prompt is dismissed as soon as it is seen, not at the end of the run.

  What this script never does, and no fix for a failure may do either: click or script "Allow",
  add, change or remove a firewall rule, or touch any other system setting. A prompt that appears
  during the run is dismissed by stopping that `PickerHost.exe` process, which is the same as
  pressing Cancel; `-KeepPrompts` leaves them up instead, and leaves the machine in the state that
  caused the outage, so use it only while diagnosing one with someone at the desktop.

  A grown count names the binary that was running when the prompt appeared. The fix belongs in
  that binary or its dependency — bind 127.0.0.1, or do not listen — never in the firewall.

      pwsh tools/ci/check-no-listeners.ps1 [-Preset msvc-release] [-Build]
                                           [-TimeoutSeconds 30] [-SettleSeconds 6]
                                           [-KeepPrompts] [-KeepTemp]
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
  [switch]$Build,
  [int]$TimeoutSeconds = 30,
  [int]$SettleSeconds = 6,
  [switch]$KeepPrompts,
  [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'

if (-not ($IsWindows -or ($env:OS -eq 'Windows_NT'))) {
  Write-Host 'check-no-listeners: Windows only (the firewall prompt is a Windows behaviour); skipping.'
  exit 0
}

Import-Module (Join-Path $PSScriptRoot '../lib/NoListeners.psm1') -Force

$buildDir = Join-Path $Root "build/$Preset"

if ($Build) {
  & (Join-Path $Root 'tools/dev.ps1') build -Preset $Preset
  if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}
if (-not (Test-Path $buildDir)) { throw "no build at $buildDir; pass -Build or build it first" }

function Get-GpuClientCount {
  $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
  if (-not $smi) { return $null }
  $out = & $smi --query-compute-apps=pid --format=csv,noheader 2>$null
  if ($LASTEXITCODE -ne 0) { return $null }
  return @($out | Where-Object { $_.Trim() }).Count
}

$exes = @(Get-ChildItem -Path $buildDir -Recurse -Include *.exe -File |
  # `_deps` holds downloaded third-party binaries (the Slang release's slangc, slangd and friends),
  # which this build did not produce. slangc is run by the shader hot reload and is a compiler with
  # no socket; slangd is a language server this tree never starts, and starting it here would be
  # inventing a listener rather than finding one. They are audited by inspection instead
  # (docs/ci/self-hosted-runners.md). `CMakeFiles` holds the compiler-identification programs
  # configure built, which are CMake's and not ours.
  Where-Object { $_.FullName -notmatch '[\\/](_deps|CMakeFiles)[\\/]' } |
  Sort-Object FullName)

if ($exes.Count -eq 0) { throw "no executables under $buildDir" }

$fresh = Join-Path ([IO.Path]::GetTempPath()) "engine-listen-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $fresh | Out-Null
Write-Host "check-no-listeners: $($exes.Count) executable(s) from $Preset"
Write-Host "check-no-listeners: fresh paths under $fresh"

$before = @(Get-FirewallPrompts)
$gpuBefore = Get-GpuClientCount
Write-Host "check-no-listeners: firewall prompts before: $($before.Count)"
if ($null -ne $gpuBefore) { Write-Host "check-no-listeners: GPU client processes before: $gpuBefore" }

$knownPids = [System.Collections.Generic.HashSet[int]]::new()
foreach ($p in $before) { [void]$knownPids.Add([int]$p.ProcessId) }

$newPrompts = New-Object System.Collections.Generic.List[object]
$observed = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
$sampled = 0

# A prompt that was not up before the run is recorded against whoever was running, and taken down
# at once (unless -KeepPrompts): one left up for the rest of the run is a GPU context held for it.
function Receive-NewPrompts([string]$blame) {
  foreach ($prompt in @(Get-FirewallPrompts)) {
    if (-not $knownPids.Add([int]$prompt.ProcessId)) { continue }
    $newPrompts.Add([pscustomobject]@{ ProcessId = [int]$prompt.ProcessId; Blame = $blame })
    if ($KeepPrompts) {
      Write-Host "  prompt PickerHost pid $($prompt.ProcessId) left up (-KeepPrompts); it holds a GPU context" -ForegroundColor Red
    } else {
      # The same as pressing Cancel. Never "Allow", and never a firewall rule.
      Write-Host "  dismissing PickerHost pid $($prompt.ProcessId) (equivalent to Cancel), raised while running $blame" -ForegroundColor Red
      Stop-Process -Id $prompt.ProcessId -Force -ErrorAction SilentlyContinue
    }
  }
}

# Every process whose image is under the run's directory is this run's: end it. Called after each
# binary, so a child a copy left behind does not outlive it, and from `finally`, so an error in
# the loop does not leave one running either.
function Stop-OwnProcesses([string]$why) {
  $own = @(Get-OwnProcesses -Under $fresh)
  foreach ($p in $own) {
    $notes.Add("ended $([IO.Path]::GetFileName($p.ExecutablePath)) (pid $($p.ProcessId)), $why")
    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
  }
  foreach ($p in $own) { Wait-Process -Id $p.ProcessId -Timeout 5 -ErrorAction SilentlyContinue }
}

$basePath = $env:PATH
$lockSwitch = $env:ENGINE_GPU_LOCK_ON_DEVICE
if (-not $lockSwitch) { $env:ENGINE_GPU_LOCK_ON_DEVICE = '1' }
$oldErrorMode = Set-QuietErrorMode

try {
  foreach ($exe in $exes) {
    # One subdirectory per binary keeps two exes of the same name apart and keeps every path new.
    $slot = Join-Path $fresh ([guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $slot | Out-Null
    $copy = Join-Path $slot $exe.Name
    Copy-Item -LiteralPath $exe.FullName -Destination $copy

    $name = [IO.Path]::GetFileNameWithoutExtension($exe.Name)
    $probeArgs = @(Get-ProbeArgs -Name $name -Scratch $slot)
    $sp = @{ FilePath = $copy; PassThru = $true; NoNewWindow = $true
             RedirectStandardOutput = (Join-Path $slot 'out.txt')
             RedirectStandardError = (Join-Path $slot 'err.txt') }
    if ($probeArgs.Count -gt 0) { $sp.ArgumentList = $probeArgs }

    # The loader looks beside the image first and on PATH last; the copy is alone in its directory,
    # so the build directory it came from goes on PATH for the DLLs it needs.
    $env:PATH = "$($exe.DirectoryName);$basePath"
    try { $proc = Start-Process @sp } finally { $env:PATH = $basePath }

    # Sample the live process before waiting on it. One sample is enough for the listener this
    # guards against, which is opened during start-up and held for the process's whole life.
    $seen = @()
    $sawProcess = $false
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
      if ($proc.HasExited -and $attempt -gt 0) { break }
      $sawProcess = $true
      $seen += Get-ProcessListeners $proc.Id
      if ($proc.HasExited) { break }
    }

    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
      # Once more before it is ended: a listener opened late (after a device, say) is still seen.
      $seen += Get-ProcessListeners $proc.Id
      $notes.Add("$name did not exit within ${TimeoutSeconds}s; stopped")
      try { $proc.Kill($true) } catch { }
      $proc.WaitForExit(5000) | Out-Null
    } elseif ($proc.ExitCode -lt 0) {
      # 0xC0000135 and friends: the process never reached main, so it never answered the question.
      $notes.Add(("$name exited 0x{0:X8} - it may not have started at all" -f $proc.ExitCode))
    }
    if ($sawProcess) { $sampled++ }
    foreach ($endpoint in ($seen | Sort-Object -Unique)) {
      $observed.Add([pscustomobject]@{ Name = $name; Endpoint = $endpoint })
    }
    Stop-OwnProcesses "left running after $name"

    # Blame is assigned while the run is still fresh: a prompt appears within a second or two of the
    # bind, so the binary that was running is the binary that listened.
    Receive-NewPrompts $name
  }

  # Prompts are raised asynchronously; give the last binary's a chance to show up.
  Start-Sleep -Seconds $SettleSeconds
  Receive-NewPrompts '(after the run)'
  $after = @(Get-FirewallPrompts)
  $gpuAfter = Get-GpuClientCount
  $verdict = Resolve-ListenerVerdict -Observed $observed.ToArray() -NewPrompts $newPrompts.ToArray() -Executables $exes.Count

  Write-Host ''
  foreach ($n in @($notes) + @($verdict.Notes)) { Write-Host "  note: $n" -ForegroundColor Yellow }
  Write-Host "check-no-listeners: firewall prompts after: $($after.Count) (before: $($before.Count); $($newPrompts.Count) raised during the run$(if (-not $KeepPrompts) { ', each dismissed' }))"
  Write-Host "check-no-listeners: sampled the live endpoints of $sampled of $($exes.Count) processes"
  if ($null -ne $gpuAfter) { Write-Host "check-no-listeners: GPU client processes after: $gpuAfter" }
  foreach ($l in $verdict.Lines) {
    Write-Host $l -ForegroundColor $(if ($verdict.Code -eq 0) { 'Green' } else { 'Red' })
  }
  exit $verdict.Code
} finally {
  Stop-OwnProcesses 'at the end of the run'
  Restore-ErrorMode $oldErrorMode
  if (-not $lockSwitch) { Remove-Item Env:ENGINE_GPU_LOCK_ON_DEVICE -ErrorAction SilentlyContinue }
  $env:PATH = $basePath
  if ($KeepTemp) {
    Write-Host "kept $fresh"
  } else {
    Remove-Item -Recurse -Force -LiteralPath $fresh -ErrorAction SilentlyContinue
  }
}
