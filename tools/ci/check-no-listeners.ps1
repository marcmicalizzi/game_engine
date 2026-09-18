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
  one appears.

  Windows raises the prompt **once per executable path**, and a path it has already decided about
  is silent forever after — which is exactly what makes the problem invisible on a developer's
  machine and fatal on a fresh one. So the check copies every executable the build produced into a
  directory with a random name, making every path new to the firewall, runs each one once with
  harmless arguments, and counts the prompt processes before and after. The count must not grow.

  What this script never does, and no fix for a failure may do either: click or script "Allow",
  add, change or remove a firewall rule, or touch any other system setting. A prompt that our own
  run raised is dismissed by stopping that `PickerHost.exe` process, which is the same as pressing
  Cancel; `-KeepPrompts` leaves them up instead, and leaves the machine in the state that caused
  the outage, so use it only while diagnosing one.

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

$buildDir = Join-Path $Root "build/$Preset"

if ($Build) {
  & (Join-Path $Root 'tools/dev.ps1') build -Preset $Preset
  if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}
if (-not (Test-Path $buildDir)) { throw "no build at $buildDir; pass -Build or build it first" }

# Every pending prompt is one of these, and its command line is what tells it apart from the other
# things Windows runs under the same image name.
function Get-FirewallPrompts {
  @(Get-CimInstance Win32_Process -Filter "Name='PickerHost.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -match 'FirewallNotificationDialogServer' })
}

# The prompt count answers "would a fresh machine be asked?", which is the question that matters,
# but only while the path really is new. This is the direct observation beside it: while the
# process is alive, what is it listening on? It needs no prompt and no fresh path, so it still
# means something on a machine that has already decided about every path in the tree — and it is
# the check that stays honest if Windows ever changes when it asks. A process that exits in
# milliseconds may not be sampled at all, which is why the run says how many it managed.
function Get-ProcessListeners([int]$processId) {
  $found = @()
  foreach ($c in @(Get-NetTCPConnection -OwningProcess $processId -State Listen -ErrorAction SilentlyContinue)) {
    $found += "TCP $($c.LocalAddress):$($c.LocalPort)"
  }
  foreach ($u in @(Get-NetUDPEndpoint -OwningProcess $processId -ErrorAction SilentlyContinue)) {
    $found += "UDP $($u.LocalAddress):$($u.LocalPort)"
  }
  return $found
}

function Test-LoopbackAddress([string]$endpoint) {
  return $endpoint -match ':127\.0\.0\.1:' -or $endpoint -match '\s127\.0\.0\.1:' -or
         $endpoint -match '\s::1:' -or $endpoint -match '\[::1\]'
}

function Get-GpuClientCount {
  $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
  if (-not $smi) { return $null }
  $out = & $smi --query-compute-apps=pid --format=csv,noheader 2>$null
  if ($LASTEXITCODE -ne 0) { return $null }
  return @($out | Where-Object { $_.Trim() }).Count
}

# Harmless arguments: enough to get the process through static initialization and into main, which
# is where a listener would already have been opened, and not enough to do any work. A Tracy client
# opens its socket before main runs, so even a binary that rejects these arguments has answered the
# question by the time it prints its usage.
function Get-ProbeArgs([string]$name) {
  switch -Regex ($name) {
    '_bench$'      { return @('--smoke', '--quiet', '--no-pin') }
    '^engine-view$' { return @('--frames', '1', '--width', '64', '--height', '64') }
    '_tests$'      { return @('--help') }
    default        { return @('--help') }
  }
}

$exes = Get-ChildItem -Path $buildDir -Recurse -Include *.exe -File |
  # `_deps` holds downloaded third-party binaries (the Slang release's slangc, slangd and friends),
  # which this build did not produce. slangc is run by the shader hot reload and is a compiler with
  # no socket; slangd is a language server this tree never starts, and starting it here would be
  # inventing a listener rather than finding one. They are audited by inspection instead
  # (docs/ci/self-hosted-runners.md).
  Where-Object { $_.FullName -notmatch '[\\/]_deps[\\/]' } |
  Sort-Object FullName

if ($exes.Count -eq 0) { throw "no executables under $buildDir" }

$fresh = Join-Path ([IO.Path]::GetTempPath()) "engine-listen-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $fresh | Out-Null
Write-Host "check-no-listeners: $($exes.Count) executable(s) from $Preset"
Write-Host "check-no-listeners: fresh paths under $fresh"

$before = Get-FirewallPrompts
$gpuBefore = Get-GpuClientCount
Write-Host "check-no-listeners: firewall prompts before: $($before.Count)"
if ($null -ne $gpuBefore) { Write-Host "check-no-listeners: GPU client processes before: $gpuBefore" }

$knownPids = [System.Collections.Generic.HashSet[int]]::new()
foreach ($p in $before) { [void]$knownPids.Add([int]$p.ProcessId) }

$blamed = @()
$notes = New-Object System.Collections.Generic.List[string]
$openListeners = New-Object System.Collections.Generic.List[string]
$sampled = 0

try {
  foreach ($exe in $exes) {
    # One subdirectory per binary keeps two exes of the same name apart and keeps every path new.
    $slot = Join-Path $fresh ([guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $slot | Out-Null
    $copy = Join-Path $slot $exe.Name
    Copy-Item -LiteralPath $exe.FullName -Destination $copy

    $name = [IO.Path]::GetFileNameWithoutExtension($exe.Name)
    $probeArgs = Get-ProbeArgs $name
    $sp = @{ FilePath = $copy; PassThru = $true; NoNewWindow = $true
             RedirectStandardOutput = (Join-Path $slot 'out.txt')
             RedirectStandardError = (Join-Path $slot 'err.txt') }
    if ($probeArgs.Count -gt 0) { $sp.ArgumentList = $probeArgs }

    $proc = Start-Process @sp

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
    if ($sawProcess) { $sampled++ }
    foreach ($endpoint in ($seen | Sort-Object -Unique)) {
      if (Test-LoopbackAddress $endpoint) {
        $notes.Add("$name listens on loopback: $endpoint (allowed; no prompt, no reachability)")
      } else {
        $openListeners.Add("$name $endpoint")
      }
    }

    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
      $notes.Add("$name did not exit within ${TimeoutSeconds}s; stopped")
      try { $proc.Kill($true) } catch { }
      $proc.WaitForExit(5000) | Out-Null
    } elseif ($proc.ExitCode -lt 0) {
      # 0xC0000135 and friends: the process never reached main, so it never answered the question.
      $notes.Add(("$name exited 0x{0:X8} — it may not have started at all" -f $proc.ExitCode))
    }

    # Blame is assigned while the run is still fresh: a prompt appears within a second or two of the
    # bind, so the binary that was running is the binary that listened.
    foreach ($prompt in Get-FirewallPrompts) {
      if ($knownPids.Add([int]$prompt.ProcessId)) { $blamed += "$name (PickerHost pid $($prompt.ProcessId))" }
    }
  }

  # Prompts are raised asynchronously; give the last binary's a chance to show up.
  Start-Sleep -Seconds $SettleSeconds
  $after = Get-FirewallPrompts
  foreach ($prompt in $after) {
    if ($knownPids.Add([int]$prompt.ProcessId)) { $blamed += "(after the run) PickerHost pid $($prompt.ProcessId)" }
  }
  $gpuAfter = Get-GpuClientCount

  Write-Host ''
  foreach ($n in $notes) { Write-Host "  note: $n" -ForegroundColor Yellow }
  Write-Host "check-no-listeners: firewall prompts after: $($after.Count) (before: $($before.Count))"
  Write-Host "check-no-listeners: sampled the live endpoints of $sampled of $($exes.Count) processes"
  if ($null -ne $gpuAfter) { Write-Host "check-no-listeners: GPU client processes after: $gpuAfter" }

  $new = @($after | Where-Object { $_.ProcessId -notin ($before | ForEach-Object { $_.ProcessId }) })

  if ($openListeners.Count -gt 0) {
    Write-Host "check-no-listeners: $($openListeners.Count) non-loopback endpoint(s) observed" -ForegroundColor Red
    foreach ($l in $openListeners) { Write-Host "  $l" -ForegroundColor Red }
  }

  if ($new.Count -eq 0 -and $openListeners.Count -eq 0) {
    Write-Host "check-no-listeners: OK — no new firewall prompt from $($exes.Count) new executable paths, and no non-loopback endpoint" -ForegroundColor Green
    exit 0
  }
  if ($new.Count -eq 0) {
    Write-Host '  The fix belongs in the binary or its dependency: bind 127.0.0.1, or do not listen.' -ForegroundColor Red
    exit 1
  }

  Write-Host "check-no-listeners: $($new.Count) new firewall prompt(s)" -ForegroundColor Red
  foreach ($b in $blamed) { Write-Host "  raised while running: $b" -ForegroundColor Red }
  if ($KeepPrompts) {
    Write-Host '  left up (-KeepPrompts). Dismiss them: every one holds a GPU context.' -ForegroundColor Red
  } else {
    # The same as pressing Cancel. Never "Allow", and never a firewall rule.
    foreach ($p in $new) {
      Write-Host "  dismissing PickerHost pid $($p.ProcessId) (equivalent to Cancel)"
      Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
  }
  Write-Host '  The fix belongs in the binary or its dependency: bind 127.0.0.1, or do not listen.' -ForegroundColor Red
  exit 1
} finally {
  if ($KeepTemp) {
    Write-Host "kept $fresh"
  } else {
    Remove-Item -Recurse -Force -LiteralPath $fresh -ErrorAction SilentlyContinue
  }
}
