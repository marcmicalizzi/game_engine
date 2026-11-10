#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for the no-listeners check (tools/lib/NoListeners.psm1, tools/ci/check-no-listeners.ps1).
  Runs under CTest as `tools.check_no_listeners`, on Windows only.

.DESCRIPTION
  The check itself starts every executable of a build from a fresh path and runs once, at the
  merge (docs/ci/self-hosted-runners.md, "tools/ci/check-no-listeners.ps1"). What it decides is held
  here as rules, with no engine binary, no firewall prompt and no listener off loopback:

    - which endpoints are loopback (127.0.0.0/8 and ::1) and which are every interface or a LAN
      address, and the verdict over a run's observations and prompts;
    - which processes are the run's own (an image under the run's directory, never a prefix of it);
    - that the probe arguments open no window and run no bench;
    - the live detection, against a stub: this test's own process opens a TCP listener and a UDP
      socket on 127.0.0.1, and Get-ProcessListeners has to see both — loopback, so no prompt;
    - that the error mode the copies inherit is set and put back;
    - that the script, given a build with nothing of its own to run, refuses before it starts
      anything, and skips CMake's and the downloaded dependencies' executables.

  Nothing here binds a non-loopback address: that is the one thing this file must never do on the
  machine it guards.

      pwsh tools/ci/check-no-listeners.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

if (-not ($IsWindows -or ($env:OS -eq 'Windows_NT'))) {
  Write-Host 'check-no-listeners tests: Windows only; skipping.'
  exit 0
}

Import-Module (Join-Path $PSScriptRoot '../lib/NoListeners.psm1') -Force
$script = Join-Path $PSScriptRoot 'check-no-listeners.ps1'

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { Write-Host "       threw: $($_.Exception.Message)"; $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) "engine-listen-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

try {
  Write-Host 'check-no-listeners tests'

  # Loopback, as the endpoints are written.
  Test-That '127.0.0.1 is loopback' { Test-LoopbackAddress 'TCP 127.0.0.1:8086' }
  Test-That 'the rest of 127.0.0.0/8 is loopback' { Test-LoopbackAddress 'UDP 127.4.0.9:5353' }
  Test-That '::1 is loopback' { Test-LoopbackAddress 'TCP ::1:8087' }
  Test-That '[::1] is loopback' { Test-LoopbackAddress 'TCP [::1]:8087' }
  Test-That '0.0.0.0 is every interface' { -not (Test-LoopbackAddress 'TCP 0.0.0.0:8086') }
  Test-That ':: is every interface' { -not (Test-LoopbackAddress 'TCP :::8086') }
  Test-That 'a LAN address is not loopback' { -not (Test-LoopbackAddress 'UDP 192.168.1.20:5353') }
  Test-That 'an address that starts like loopback is not' { -not (Test-LoopbackAddress 'TCP 1127.0.0.1:80') }
  Test-That 'a malformed endpoint is not loopback' { -not (Test-LoopbackAddress '127.0.0.1') }

  # The verdict.
  $clean = Resolve-ListenerVerdict -Observed @() -NewPrompts @() -Executables 3
  Test-That 'nothing observed passes' { $clean.Code -eq 0 -and ($clean.Lines -join "`n").Contains('OK') }
  $loop = Resolve-ListenerVerdict -Observed @([pscustomobject]@{ Name = 'engine-host'; Endpoint = 'TCP 127.0.0.1:50123' }) -Executables 1
  Test-That 'a loopback listener passes, with a note naming it' {
    $loop.Code -eq 0 -and ($loop.Notes -join "`n").Contains('engine-host listens on loopback: TCP 127.0.0.1:50123')
  }
  $open = Resolve-ListenerVerdict -Observed @(
    [pscustomobject]@{ Name = 'engine-host'; Endpoint = 'TCP 127.0.0.1:50123' },
    [pscustomobject]@{ Name = 'engine_foo_bench'; Endpoint = 'TCP 0.0.0.0:8086' }) -Executables 2
  Test-That 'a listener on every interface fails, naming the binary and the endpoint' {
    $open.Code -eq 1 -and ($open.Lines -join "`n").Contains('engine_foo_bench TCP 0.0.0.0:8086')
  }
  Test-That 'and says where the fix belongs' { ($open.Lines -join "`n").Contains('bind 127.0.0.1, or do not listen') }
  $prompted = Resolve-ListenerVerdict -NewPrompts @([pscustomobject]@{ ProcessId = 4242; Blame = 'engine-view' }) -Executables 1
  Test-That 'a new prompt fails, naming the binary it was raised under' {
    $prompted.Code -eq 1 -and ($prompted.Lines -join "`n").Contains('raised while running: engine-view (PickerHost pid 4242)')
  }

  # Prompts, from a process list written here.
  $fakePickers = @(
    [pscustomobject]@{ ProcessId = 10; CommandLine = 'C:\Windows\System32\PickerHost.exe -ServerName:FirewallNotificationDialogServer' },
    [pscustomobject]@{ ProcessId = 11; CommandLine = 'C:\Windows\System32\PickerHost.exe -ServerName:SomethingElse' },
    [pscustomobject]@{ ProcessId = 12; CommandLine = $null })
  $prompts = @(Get-FirewallPrompts -Processes $fakePickers)
  Test-That 'only the firewall dialog server is a prompt' { $prompts.Count -eq 1 -and $prompts[0].ProcessId -eq 10 }

  # The run's own processes, by image path.
  $under = Join-Path $scratch 'engine-listen-0123'
  $fakeProcs = @(
    [pscustomobject]@{ ProcessId = 1; ExecutablePath = (Join-Path $under 'ab12cd34\engine-view.exe') },
    [pscustomobject]@{ ProcessId = 2; ExecutablePath = ((Join-Path $under 'ef56\ENGINE-HOST.EXE').ToUpperInvariant()) },
    [pscustomobject]@{ ProcessId = 3; ExecutablePath = ($under + '-other\engine-view.exe') },
    [pscustomobject]@{ ProcessId = 4; ExecutablePath = 'C:\Windows\System32\svchost.exe' },
    [pscustomobject]@{ ProcessId = 5; ExecutablePath = $null })
  $own = @(Get-OwnProcesses -Under $under -Processes $fakeProcs | ForEach-Object { $_.ProcessId })
  Test-That "a process whose image is under the run's directory is the run's, in any case" { ($own -join ',') -eq '1,2' }
  Test-That 'a directory that only starts with the same name is not' { $own -notcontains 3 }

  # The probes open no window and run no bench.
  $view = @(Get-ProbeArgs -Name 'engine-view' -Scratch $scratch)
  $editor = @(Get-ProbeArgs -Name 'engine-editor' -Scratch $scratch)
  Test-That 'engine-view runs offscreen' { $view -contains '--offscreen' }
  # engine-editor refuses --offscreen with --listen (apps/engine_editor/main.cpp), and a refused
  # probe opens no listener; so its probe has a small window, no shell, and the listener open.
  Test-That 'engine-editor runs with its listener on loopback, no shell, and never --offscreen with --listen' {
    ($editor -notcontains '--offscreen') -and ($editor -contains '--no-shell') -and
      ((($editor -join ' ')).Contains('--listen 127.0.0.1:0'))
  }
  $hostArgs = @(Get-ProbeArgs -Name 'engine-host' -Scratch $scratch)
  Test-That 'engine-host listens on loopback only, and is held open rather than answering one request' {
    (($hostArgs -join ' ') -eq '--listen 127.0.0.1:0') -and (Test-HoldsUntilListening 'engine-host')
  }
  Test-That 'only engine-host is held by its input' {
    -not (Test-HoldsUntilListening 'engine-editor') -and -not (Test-HoldsUntilListening 'engine-view')
  }
  Test-That 'the known listeners are engine-host and engine-editor' {
    ((@(Get-KnownListeners) | Sort-Object) -join ',') -eq 'engine-editor,engine-host'
  }

  # The positive control: a run whose sampler saw no known listener must not print OK.
  $hostSeen = [pscustomobject]@{ Name = 'engine-host'; Endpoint = 'TCP 127.0.0.1:50123' }
  $editorSeen = [pscustomobject]@{ Name = 'engine-editor'; Endpoint = 'TCP 127.0.0.1:50124' }
  $blind = Resolve-ListenerVerdict -Observed @() -Started @('engine-host', 'engine-editor', 'engine_math_tests') -Executables 3
  Test-That 'a run that started the known listeners and saw neither fails' { $blind.Code -eq 1 }
  Test-That 'and says the sampler saw nothing where a listener is known, naming each binary' {
    $text = $blind.Lines -join "`n"
    $text.Contains('the sampler saw nothing where a listener is known: engine-host') -and
      $text.Contains('the sampler saw nothing where a listener is known: engine-editor')
  }
  $half = Resolve-ListenerVerdict -Observed @($hostSeen) -Started @('engine-host', 'engine-editor') -Executables 2
  Test-That 'one known listener seen and the other not still fails, naming the one missed' {
    $half.Code -eq 1 -and ($half.Lines -join "`n").Contains('known: engine-editor') -and -not ($half.Lines -join "`n").Contains('known: engine-host')
  }
  $offLoop = Resolve-ListenerVerdict -Observed @([pscustomobject]@{ Name = 'engine-host'; Endpoint = 'TCP 0.0.0.0:50123' }), $editorSeen -Started @('engine-host', 'engine-editor') -Executables 2
  Test-That 'a known listener seen only off loopback counts as not seen on loopback, and fails twice over' {
    $offLoop.Code -eq 1 -and ($offLoop.Lines -join "`n").Contains('known: engine-host') -and ($offLoop.Lines -join "`n").Contains('engine-host TCP 0.0.0.0:50123')
  }
  $both = Resolve-ListenerVerdict -Observed @($hostSeen, $editorSeen) -Started @('engine-host', 'engine-editor') -Executables 2
  Test-That 'both known listeners seen on loopback passes' { $both.Code -eq 0 }
  $absent = Resolve-ListenerVerdict -Observed @() -Started @('engine_math_tests') -Executables 1
  Test-That 'a build without the known listeners is not held to them' { $absent.Code -eq 0 }
  Test-That 'a bench lists its cases and runs none' { (@(Get-ProbeArgs -Name 'engine_math_bench' -Scratch $scratch) -join ' ') -eq '--list' }
  Test-That 'the GPU lock probe runs a command that holds no lock' { (@(Get-ProbeArgs -Name 'engine_gpu_lock_probe' -Scratch $scratch) -join ' ') -eq 'cycles 0' }
  Test-That 'a test executable is asked for its help' { (@(Get-ProbeArgs -Name 'engine_math_tests' -Scratch $scratch) -join ' ') -eq '--help' }

  # The live half, against a stub: this process, listening on loopback. Get-NetTCPConnection and
  # Get-NetUDPEndpoint read the system's tables, so this is the same observation the check makes of
  # a copy, and loopback is the one address that raises no prompt.
  $tcp = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $udp = $null
  try {
    $tcp.Start()
    $udp = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
    $tcpPort = ([System.Net.IPEndPoint]$tcp.LocalEndpoint).Port
    $udpPort = ([System.Net.IPEndPoint]$udp.Client.LocalEndPoint).Port
    $seen = @(Get-ProcessListeners $PID)
    Test-That 'the stub''s TCP listener is seen' { $seen -contains "TCP 127.0.0.1:$tcpPort" }
    Test-That 'the stub''s UDP socket is seen' { $seen -contains "UDP 127.0.0.1:$udpPort" }
    $live = Resolve-ListenerVerdict -Observed @($seen | ForEach-Object { [pscustomobject]@{ Name = 'stub'; Endpoint = $_ } }) -Executables 1
    Test-That 'and the stub passes, because both are loopback' { $live.Code -eq 0 }
  } finally {
    $tcp.Stop()
    if ($udp) { $udp.Dispose() }
  }

  # The error mode the copies inherit: set, and put back.
  $original = Set-QuietErrorMode
  $mine = Set-QuietErrorMode
  Restore-ErrorMode $original
  $restored = Set-QuietErrorMode
  Restore-ErrorMode $original
  Test-That 'the quiet error mode is set (no loader, crash or open-file box)' { ($mine -band 0x8003) -eq 0x8003 }
  Test-That 'and the previous mode is put back' { ($restored -band 0xFFFF) -eq ($original -band 0xFFFF) }

  # The script over builds with nothing of its own to run: it refuses, and starts nothing.
  $fixture = Join-Path $scratch 'tree'
  New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'build/msvc-release/_deps/slang-src/bin') | Out-Null
  New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'build/msvc-release/CMakeFiles/4.0.0/CompilerIdCXX') | Out-Null
  # Not executables: a refusal is all that may come of them, so their bytes do not matter.
  Set-Content -LiteralPath (Join-Path $fixture 'build/msvc-release/_deps/slang-src/bin/slangd.exe') -Value 'not a program'
  Set-Content -LiteralPath (Join-Path $fixture 'build/msvc-release/CMakeFiles/4.0.0/CompilerIdCXX/CMakeCXXCompilerId.exe') -Value 'not a program'
  $out = (& pwsh -NoProfile -File $script -Root $fixture -Preset msvc-release *>&1 | Out-String)
  $code = $LASTEXITCODE
  Test-That "a build whose only executables are CMake's and _deps' has nothing to check, and says so" {
    $code -ne 0 -and $out.Contains('no executables under')
  }
  $out = (& pwsh -NoProfile -File $script -Root $fixture -Preset msvc-nothing *>&1 | Out-String)
  $code = $LASTEXITCODE
  Test-That 'a preset that was never built is refused' { $code -ne 0 -and $out.Contains('no build at') }
} finally {
  Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "check-no-listeners: $checks checks passed" -ForegroundColor Green
exit 0
