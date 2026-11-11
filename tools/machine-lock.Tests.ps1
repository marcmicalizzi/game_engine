#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/lib/MachineLock.psm1 and the tools/gpu-lock.ps1 command line over it. Runs under
  CTest as `tools.machine_lock`.

.DESCRIPTION
  The lock protocol (D:\workspace\GPU-LOCK.md) is shared with tools that are not this repository's
  — a Blender agent takes the same GPU lock from Python — so the rules are tested as rules: create
  is atomic, an unexpired lock is never broken, an expired one is, a person's never is, a release
  deletes only its own lock, a refresh is never seen half-written, and a holder that runs long
  keeps its lease alive. Every case works on a lock file in its own scratch directory; nothing
  here reads or writes the machine's real locks.

      pwsh tools/machine-lock.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force
$cli = Join-Path $PSScriptRoot 'gpu-lock.ps1'

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$scratch = Join-Path ([IO.Path]::GetTempPath()) "engine-machine-lock-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { Write-Host "       threw: $($_.Exception.Message)"; $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

$case = 0
function New-LockPath { $script:case++; return (Join-Path $scratch "case$($script:case).lock") }

function Write-Lock([string]$path, [string]$owner, [int]$minutesFromNow, [long]$lockPid = 4321) {
  $now = [DateTime]::UtcNow
  $body = [ordered]@{
    owner = $owner; purpose = "held by a test"; pid = $lockPid
    started = (ConvertTo-MachineLockTime $now.AddMinutes(-5))
    expires = (ConvertTo-MachineLockTime $now.AddMinutes($minutesFromNow)); host = 'TESTHOST'
  } | ConvertTo-Json -Compress
  [IO.File]::WriteAllText($path, $body)
}

try {
  Write-Host 'acquire and release'
  $p = New-LockPath
  $r = Enter-MachineLock -Path $p -Purpose 'test one' -Owner 'claude-engine' -LeaseMinutes 10
  Test-That 'a free lock is taken' { $r.Outcome -eq 'Taken' -and (Test-Path -LiteralPath $p) }
  $seen = Read-MachineLock -Path $p
  Test-That 'the file carries the protocol fields' {
    $seen.Readable -and $seen.Owner -eq 'claude-engine' -and $seen.Purpose -eq 'test one' -and
    $seen.Pid -eq $PID -and $seen.Started -and $seen.ExpiresText -match 'Z$' -and $seen.Blocking
  }
  Test-That 'the expiry is the lease from now' {
    $d = ($seen.Expires - [DateTime]::UtcNow).TotalMinutes; $d -gt 9.5 -and $d -le 10.1
  }
  $second = Enter-MachineLock -Path $p -Purpose 'test two' -Owner 'claude-engine'
  Test-That 'a held lock is busy for a second taker, who is told who holds it' {
    $second.Outcome -eq 'Busy' -and $second.Holder.Purpose -eq 'test one'
  }
  Exit-MachineLock -Handle $r.Handle
  Test-That 'release deletes it' { -not (Test-Path -LiteralPath $p) }

  Write-Host 'expiry'
  $p = New-LockPath
  Write-Lock $p 'astra-blender' -1
  $r = Enter-MachineLock -Path $p -Purpose 'after an expired one' -Owner 'claude-engine' -Label 'test'
  Test-That 'an expired lock is broken and taken' { $r.Outcome -eq 'Taken' -and (Read-MachineLock -Path $p).Owner -eq 'claude-engine' }
  Exit-MachineLock -Handle $r.Handle

  $p = New-LockPath
  Write-Lock $p 'astra-blender' 30
  $before = [IO.File]::ReadAllText($p)
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine'
  Test-That 'an unexpired lock is never broken' { $r.Outcome -eq 'Busy' -and [IO.File]::ReadAllText($p) -eq $before }

  $p = New-LockPath
  Write-Lock $p 'marc' -60
  $before = [IO.File]::ReadAllText($p)
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine'
  Test-That "a person's lock is never broken, even expired" { $r.Outcome -eq 'Busy' -and [IO.File]::ReadAllText($p) -eq $before }

  $p = New-LockPath
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -Wait -TimeoutMinutes 0
  Exit-MachineLock -Handle $r.Handle
  Write-Lock $p 'astra-blender' 30
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -Wait -TimeoutMinutes 0.02 -PollSeconds 1
  Test-That 'a waiter gives up at its deadline and says who held it' { $r.Outcome -eq 'TimedOut' -and $r.Holder.Owner -eq 'astra-blender' }

  Write-Host 'unreadable files'
  $p = New-LockPath
  [IO.File]::WriteAllText($p, '{"owner":"astra-')
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine'
  Test-That 'a half-written lock is being written, not expired' { $r.Outcome -eq 'Busy' -and -not (Read-MachineLock -Path $p).Expired }
  [IO.File]::SetLastWriteTimeUtc($p, [DateTime]::UtcNow.AddMinutes(-5))
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine'
  Test-That 'an unreadable lock older than a minute is garbage and is broken' { $r.Outcome -eq 'Taken' }
  Exit-MachineLock -Handle $r.Handle

  Write-Host 'refresh and release'
  $p = New-LockPath
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -LeaseMinutes 1
  $first = (Read-MachineLock -Path $p).Expires
  Test-That 'a refresh pushes the expiry out by the lease it names' {
    (Update-MachineLock -Handle $r.Handle -LeaseMinutes 30) -and ((Read-MachineLock -Path $p).Expires - $first).TotalMinutes -gt 25
  }
  Test-That 'a refreshed lock is still the same holder' { (Read-MachineLock -Path $p).Started -eq $r.Handle.Started }
  Write-Lock $p 'astra-blender' 30
  Test-That 'a refresh of a lock that is no longer ours does nothing' { -not (Update-MachineLock -Handle $r.Handle) }
  Exit-MachineLock -Handle $r.Handle
  Test-That "a release does not delete somebody else's lock" { (Read-MachineLock -Path $p).Owner -eq 'astra-blender' }

  # Every engine agent is "claude-engine": a release by owner alone would delete another agent's
  # lock. The handle's pid and start time are what make a lock this holder's.
  $p = New-LockPath
  Write-Lock $p 'claude-engine' 30 999999937
  $mine = [pscustomobject]@{ Path = $p; Owner = 'claude-engine'; Purpose = 'x'; Pid = [long]$PID
                             Started = 'never'; ExpiresText = ''; LeaseMinutes = 10; Host = 'TESTHOST' }
  Exit-MachineLock -Handle $mine
  Test-That "a release does not delete another agent's lock under the same owner" { Test-Path -LiteralPath $p }

  Write-Host 'atomicity'
  $p = New-LockPath
  $module = Join-Path $PSScriptRoot 'lib/MachineLock.psm1'
  $jobs = 1..8 | ForEach-Object {
    Start-ThreadJob -ArgumentList $module, $p, $_ -ScriptBlock {
      param($module, $path, $n)
      Import-Module $module
      (Enter-MachineLock -Path $path -Purpose "racer $n" -Owner 'claude-engine').Outcome
    }
  }
  $outcomes = @($jobs | Wait-Job | Receive-Job)
  $jobs | Remove-Job -Force
  Test-That 'eight simultaneous takers: exactly one wins' { @($outcomes | Where-Object { $_ -eq 'Taken' }).Count -eq 1 -and $outcomes.Count -eq 8 }

  Write-Host 'heartbeat and the wrapper'
  $p = New-LockPath
  $r = Enter-MachineLock -Path $p -Purpose 'long' -Owner 'claude-engine' -LeaseMinutes 0.05   # 3 s
  $firstExpiry = (Read-MachineLock -Path $p).Expires
  $beat = Start-MachineLockHeartbeat -Handle $r.Handle -IntervalSeconds 1
  Start-Sleep -Seconds 5
  $later = Read-MachineLock -Path $p
  Test-That 'a heartbeat keeps a short lease alive past its first expiry' { $later.Blocking -and $later.Expires -gt $firstExpiry }
  Stop-MachineLockHeartbeat -Job $beat
  Exit-MachineLock -Handle $r.Handle
  Test-That 'and the lock is gone once the heartbeat stops and the holder releases' { -not (Test-Path -LiteralPath $p) }

  $p = New-LockPath
  $inside = $null
  Invoke-WithMachineLock -Kind gpu -Path $p -Purpose 'wrapped' -Owner 'claude-engine' -ScriptBlock {
    $script:inside = [pscustomobject]@{ Held = (Read-MachineLock -Path $p); Holder = $env:ENGINE_GPU_LOCK_HOLDER }
  }
  Test-That 'Invoke-WithMachineLock holds the lock while the block runs' { $inside.Held.Purpose -eq 'wrapped' -and $inside.Held.Pid -eq $PID }
  Test-That 'and tells a bench inside it the lock is held on its behalf' { $inside.Holder -eq "$PID" }
  Test-That 'and releases it after' { -not (Test-Path -LiteralPath $p) }
  try {
    Invoke-WithMachineLock -Kind gpu -Path $p -Purpose 'throws' -Owner 'claude-engine' -ScriptBlock { throw 'inside' }
  } catch { }
  Test-That 'and releases it when the block throws' { -not (Test-Path -LiteralPath $p) }

  # A gate that names its owner with -Owner, not in its environment: the children decide "held
  # for me" by owner and pid together, so the session hands its owner down with its pid.
  $p = New-LockPath
  $ownerBefore = $env:ENGINE_GPU_LOCK_OWNER
  $holderBefore = $env:ENGINE_GPU_LOCK_HOLDER
  $inside = $null
  Invoke-WithMachineLock -Kind gpu -Path $p -Purpose 'gate' -Owner 'fable-merge-test' -ScriptBlock {
    $script:inside = [pscustomobject]@{ Owner = $env:ENGINE_GPU_LOCK_OWNER; Holder = $env:ENGINE_GPU_LOCK_HOLDER }
  }
  Test-That 'a session hands its owner down with its pid' { $inside.Owner -eq 'fable-merge-test' -and $inside.Holder -eq "$PID" }
  Test-That 'and puts both back when it closes' {
    $env:ENGINE_GPU_LOCK_OWNER -eq $ownerBefore -and $env:ENGINE_GPU_LOCK_HOLDER -eq $holderBefore
  }

  # A session inside a session (gpu-smoke.ps1 inside `gpu-lock.ps1 run`): held on this run's
  # behalf already, so it neither waits for its own parent nor takes anything. -NoWait makes a
  # wait an error, so this fails loudly if it does not recognise the hold.
  $p = New-LockPath
  $nested = $null
  Invoke-WithMachineLock -Kind gpu -Path $p -Purpose 'outer' -Owner 'fable-merge-test' -ScriptBlock {
    $before = [IO.File]::ReadAllText($p)
    $inner = Open-MachineLockSession -Kind gpu -Path $p -Purpose 'inner' -Owner 'fable-merge-test' -NoWait
    $during = [IO.File]::ReadAllText($p)
    Close-MachineLockSession -Session $inner
    $script:nested = [pscustomobject]@{
      Inner = $inner; Unchanged = ($during -eq $before); StillThere = (Test-Path -LiteralPath $p)
    }
  }
  Test-That 'a session inside a session holds nothing and waits for nothing' {
    $null -ne $nested -and $null -eq $nested.Inner.Handle -and $nested.Unchanged -and $nested.StillThere
  }
  Test-That 'and the outer one still releases' { -not (Test-Path -LiteralPath $p) }

  $p = Join-Path $scratch 'no-such-directory/gpu.lock'
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine'
  Test-That 'a missing lock directory is a machine without the protocol' { $r.Outcome -eq 'NoDirectory' }

  Write-Host 'the command line'
  $p = New-LockPath
  $out = & pwsh -NoProfile -File $cli status -LockFile $p
  Test-That 'status of a free lock is "free", exit 0' { $LASTEXITCODE -eq 0 -and "$out" -eq 'free' }
  $out = & pwsh -NoProfile -File $cli acquire -Purpose 'cli test' -LockFile $p
  Test-That 'acquire takes it, exit 0' { $LASTEXITCODE -eq 0 -and (Read-MachineLock -Path $p).Purpose -eq 'cli test' }
  $out = & pwsh -NoProfile -File $cli acquire -Purpose 'cli again' -LockFile $p
  Test-That 'acquire again is busy, exit 2' { $LASTEXITCODE -eq 2 -and "$out" -match 'busy' }
  $out = & pwsh -NoProfile -File $cli status -LockFile $p
  Test-That 'status of a held lock names it, exit 2' { $LASTEXITCODE -eq 2 -and "$out" -match 'cli test' }
  $out = & pwsh -NoProfile -File $cli release -LockFile $p
  Test-That 'release frees it, exit 0' { $LASTEXITCODE -eq 0 -and -not (Test-Path -LiteralPath $p) }
  $out = & pwsh -NoProfile -File $cli run -Purpose 'cli run' -LockFile $p -Exec "if (Test-Path -LiteralPath '$p') { exit 7 } else { exit 9 }"
  Test-That 'run holds the lock around the command and returns its exit code' { $LASTEXITCODE -eq 7 }
  Test-That 'and releases it after' { -not (Test-Path -LiteralPath $p) }
  # A program's own exit code comes through, not PowerShell's "1 for anything that failed".
  $out = & pwsh -NoProfile -File $cli run -Purpose 'cli run' -LockFile $p -Exec "pwsh -NoProfile -Command 'exit 7'"
  Test-That 'run returns the exit code of the program its command ran' { $LASTEXITCODE -eq 7 }
  $out = & pwsh -NoProfile -File $cli run -Purpose 'cli run' -LockFile $p -Exec "pwsh -NoProfile -Command 'exit 0'"
  Test-That 'and 0 when the program succeeded' { $LASTEXITCODE -eq 0 }
  $out = & pwsh -NoProfile -File $cli run -Purpose 'cli run' -LockFile $p -Exec "Write-Output 'no program ran'"
  Test-That 'and 0 for a command line that ran no program' { $LASTEXITCODE -eq 0 -and "$out" -match 'no program ran' }
  if ($IsWindows) {
    $out = & pwsh -NoProfile -File $cli run -Purpose 'cli run' -LockFile $p -Exec 'cmd /c exit -1073741819'
    Test-That 'an access violation''s code (0xC0000005) comes through whole' { $LASTEXITCODE -eq -1073741819 }
  }

  # --- the line (T96) ---------------------------------------------------------------------------
  # A ticket as any implementation writes it: the name is the place, the body a lock's.
  function Write-Ticket([string]$lock, [long]$arrivedMs, [long]$declaredSeconds, [long]$ticketPid, [double]$aliveSeconds, [string]$purpose, [int]$counter = 0) {
    $dir = "$lock.line"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $name = 't-{0:D16}-{1:D16}-{2:D10}-{3:D4}.json' -f ($arrivedMs + $declaredSeconds * 1000), $arrivedMs, $ticketPid, $counter
    $body = [ordered]@{
      owner = 'claude-engine'; purpose = $purpose; pid = $ticketPid
      started = (ConvertTo-MachineLockTime ([DateTimeOffset]::FromUnixTimeMilliseconds($arrivedMs).UtcDateTime))
      expires = (ConvertTo-MachineLockTime ([DateTime]::UtcNow.AddSeconds($aliveSeconds))); host = 'TESTHOST'
      declared_s = $declaredSeconds
    } | ConvertTo-Json -Compress
    $path = Join-Path $dir $name
    [IO.File]::WriteAllText($path, $body)
    return $path
  }
  function Get-NowMs { return [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }
  # A waiter in a process of its own, as a second agent is: `gpu-lock.ps1 run` at the case's lock.
  # No window, and its output in the case's scratch rather than in this test's.
  $script:children = 0
  function Start-Child([string[]]$arguments) {
    $script:children++
    $log = Join-Path $scratch "child$($script:children).log"
    return Start-Process pwsh -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError "$log.err" -ArgumentList $arguments
  }
  function Start-Waiter([string]$lock, [string]$purpose, [string]$exec, [double]$minutes = 10) {
    return Start-Child @('-NoProfile', '-File', "`"$cli`"", 'run', '-Purpose', "`"$purpose`"", '-Minutes', "$minutes", '-LockFile', "`"$lock`"", '-Exec', "`"$exec`"")
  }
  function Wait-Line([string]$lock, [int]$count, [int]$timeoutSeconds = 60) {
    $until = [DateTime]::UtcNow.AddSeconds($timeoutSeconds)
    while ([DateTime]::UtcNow -lt $until) {
      if ((Get-MachineLockLine -Path $lock).Count -eq $count) { return $true }
      Start-Sleep -Milliseconds 50
    }
    return $false
  }

  Write-Host 'the line: tickets'
  $p = New-LockPath
  $theirs = Write-Ticket $p ((Get-NowMs) - 10000) 600 4321 30 'ahead of everyone'
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -LeaseMinutes 10
  Test-That 'a free lock is not taken past a live ticket, and the answer says where the ask stood' {
    $r.Outcome -eq 'Busy' -and -not (Test-Path -LiteralPath $p) -and -not $r.Holder.Present -and
    $r.Place.Ahead -eq 1 -and (Format-MachineLockRefusal $r) -match "free, and not this ask's turn: it was behind 1 of the 2 in the line, first 'claude-engine' \(pid 4321\): ahead of everyone, waiting 1\d s"
  }
  Test-That 'and the ask left no ticket of its own behind' { @(Get-ChildItem -LiteralPath "$p.line").Count -eq 1 }
  Remove-Item -LiteralPath $theirs -Force
  $dead = Write-Ticket $p ((Get-NowMs) - 60000) 600 4321 -5 'died in the line'
  Test-That 'a ticket whose lease ran out is not in the line' { (Get-MachineLockLine -Path $p).Count -eq 0 }
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -LeaseMinutes 10
  Test-That 'and does not hold the lock from anyone; whoever passes removes it' {
    $r.Outcome -eq 'Taken' -and -not (Test-Path -LiteralPath $dead) -and @(Get-ChildItem -LiteralPath "$p.line").Count -eq 0
  }
  Exit-MachineLock -Handle $r.Handle

  $p = New-LockPath
  $fresh = Write-Ticket $p ((Get-NowMs) - 1000) 600 4321 30 'ten minutes, just now'
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -LeaseMinutes 2
  Test-That 'a two-minute ask passes a ten-minute ask that arrived a second ago' { $r.Outcome -eq 'Taken' }
  Exit-MachineLock -Handle $r.Handle
  Remove-Item -LiteralPath $fresh -Force
  Write-Ticket $p ((Get-NowMs) - 500000) 600 4321 30 'ten minutes, waited' | Out-Null
  $r = Enter-MachineLock -Path $p -Purpose 'x' -Owner 'claude-engine' -LeaseMinutes 2
  Test-That 'and not one that has waited longer than the difference between them' { $r.Outcome -eq 'Busy' -and $r.Place.Ahead -eq 1 }

  $p = New-LockPath
  $t0 = (Get-NowMs) - 100000
  Write-Ticket $p ($t0 + 5) 120 700 30 'later key' | Out-Null
  Write-Ticket $p $t0 600 900 30 'long, first to arrive' | Out-Null
  Write-Ticket $p $t0 120 800 30 'same millisecond, higher pid' | Out-Null
  Write-Ticket $p $t0 120 300 30 'same millisecond, lower pid' | Out-Null
  Write-Ticket $p $t0 120 300 30 'same pid, its second ask' 1 | Out-Null
  Write-Ticket $p ($t0 - 1000) 121 950 30 'same key, earlier arrival' | Out-Null
  [IO.File]::WriteAllText((Join-Path "$p.line" 'README.txt'), 'not a ticket')
  $line = Get-MachineLockLine -Path $p
  Test-That 'the order is the tickets'' own: key, arrival, pid, counter (the same six the C++ test sorts)' {
    ($line | ForEach-Object Purpose) -join '|' -eq 'same key, earlier arrival|same millisecond, lower pid|same pid, its second ask|same millisecond, higher pid|later key|long, first to arrive' -and
    $line[5].DeclaredSeconds -eq 600 -and $line[5].Pid -eq 900
  }

  Write-Host 'the line: status'
  Write-Lock $p 'astra-blender' 30
  $out = & pwsh -NoProfile -File $cli status -LockFile $p
  Test-That 'status prints the holder, since when and until when, then the line in order' {
    $LASTEXITCODE -eq 2 -and $out.Count -eq 8 -and $out[0] -match "^held by 'astra-blender': held by a test \(until .+Z\) \(pid 4321, since .+Z\)$" -and
    $out[1] -eq '6 waiting, first served first:' -and
    $out[2] -match "^  1\. 'claude-engine' \(pid 950\): same key, earlier arrival, waiting 1\d\d s, asks 2 min, since .+Z$" -and
    $out[7] -match "^  6\. 'claude-engine' \(pid 900\): long, first to arrive, waiting \d+ s, asks 10 min, since .+Z$"
  }
  Remove-Item -LiteralPath $p -Force
  $out = & pwsh -NoProfile -File $cli status -LockFile $p
  Test-That 'a free lock with a line is "free", exit 0, and the line' { $LASTEXITCODE -eq 0 -and $out[0] -eq 'free' -and $out.Count -eq 8 }
  $out = & pwsh -NoProfile -File $cli acquire -Purpose 'cli, no turn' -LockFile $p -Minutes 30
  Test-That 'acquire of a free lock behind a line is busy, exit 2, and says so' {
    $LASTEXITCODE -eq 2 -and "$out" -match "^busy: free, and not this ask's turn: it was behind 6 of the 7 in the line" -and -not (Test-Path -LiteralPath $p)
  }

  Write-Host 'the line: several processes'
  # Somebody holds the lock; three waiters arrive one at a time, and a fourth process that has
  # just held the lock asks again (here: arrives last). Each appends its name while it holds.
  $p = New-LockPath
  $order = "$p.order.txt"
  Write-Lock $p 'astra-blender' 30
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $waiters = @()
  foreach ($n in 'one', 'two', 'three') {
    $waiters += Start-Waiter $p "waiter $n" "Add-Content -LiteralPath '$order' -Value '$n'; Start-Sleep -Milliseconds 300"
    if (-not (Wait-Line $p $waiters.Count)) { break }
  }
  Test-That 'three waiters stand in the line in the order they arrived' {
    ((Get-MachineLockLine -Path $p) | ForEach-Object Purpose) -join '|' -eq 'waiter one|waiter two|waiter three'
  }
  Test-That 'and nobody passed the holder' { -not (Test-Path -LiteralPath $order) }
  Remove-Item -LiteralPath $p -Force
  $waiters | ForEach-Object { $_.WaitForExit(120000) | Out-Null }
  Test-That "they are served in that order (three pwsh waiters, $([int]$sw.Elapsed.TotalSeconds) s)" {
    (Get-Content -LiteralPath $order) -join '|' -eq 'one|two|three' -and -not (Test-Path -LiteralPath $p)
  }

  # A holder that lets go and asks again goes behind whoever was waiting: `again` holds, `late`
  # arrives while it does, `again` releases and asks at once (the second run starts as the first
  # ends), and `late` is served before `again`'s second hold.
  $p = New-LockPath
  $order = "$p.order.txt"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [IO.File]::WriteAllText("$p.again.ps1", @"
& '$cli' run -Purpose again -LockFile '$p' -Exec "Add-Content -LiteralPath '$order' -Value 'again 1'; while (-not (Test-Path -LiteralPath '$p.go')) { Start-Sleep -Milliseconds 50 }"
& '$cli' run -Purpose again -LockFile '$p' -Exec "Add-Content -LiteralPath '$order' -Value 'again 2'"
"@)
  $again = Start-Child @('-NoProfile', '-File', "`"$p.again.ps1`"")
  $until = [DateTime]::UtcNow.AddSeconds(60)
  while (-not (Test-Path -LiteralPath $order) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 50 }
  $late = Start-Waiter $p 'late' "Add-Content -LiteralPath '$order' -Value 'late'; Start-Sleep -Milliseconds 300"
  $lined = Wait-Line $p 1
  [IO.File]::WriteAllText("$p.go", 'go')
  $late.WaitForExit(120000) | Out-Null; $again.WaitForExit(120000) | Out-Null
  Test-That "a holder that releases and asks again goes behind the one that was waiting ($([int]$sw.Elapsed.TotalSeconds) s)" {
    $lined -and (Get-Content -LiteralPath $order) -join '|' -eq 'again 1|late|again 2'
  }

  # A waiter killed in the line (Stop-Process on this test's own child: no finally runs, its
  # ticket stays) stops counting when its ticket's lease runs out. The doomed one asks with a
  # three-second ticket so that the case takes seconds; a tool's own is 30 s, and the rule is the
  # same one.
  $p = New-LockPath
  $order = "$p.order.txt"
  Write-Lock $p 'astra-blender' 30
  [IO.File]::WriteAllText("$p.doomed.ps1", @"
Import-Module '$module' -Force
Enter-MachineLock -Path '$p' -Purpose doomed -Owner claude-engine -LeaseMinutes 10 -Wait -TicketLeaseSeconds 3 | Out-Null
Add-Content -LiteralPath '$order' -Value 'doomed'
"@)
  $doomed = Start-Child @('-NoProfile', '-File', "`"$p.doomed.ps1`"")
  $one = Wait-Line $p 1
  $behind = Start-Waiter $p 'behind' "Add-Content -LiteralPath '$order' -Value 'behind'"
  $two = Wait-Line $p 2
  Stop-Process -Id $doomed.Id -Force
  $doomed.WaitForExit(30000) | Out-Null
  Remove-Item -LiteralPath $p -Force
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $behind.WaitForExit(120000) | Out-Null
  $waited = $sw.Elapsed.TotalSeconds
  Test-That ("a waiter killed in the line blocks the one behind it for its ticket's lease and no longer ({0:N1} s; the bound is 4 s and the machine)" -f $waited) {
    $one -and $two -and $behind.HasExited -and $waited -lt 4 + 0.25 + 10 -and (Get-Content -LiteralPath $order) -join '|' -eq 'behind'
  }
  Test-That 'and its ticket is gone, removed by the one that passed it' { (Get-MachineLockLine -Path $p).Count -eq 0 -and @(Get-ChildItem -LiteralPath "$p.line" -Filter 't-*.json').Count -eq 0 }

  Write-Host 'the build lock, through tools/linux-build.ps1 -LockOnly'
  # The container build's lock is this protocol at another path, taken by linux-build.ps1; its
  # line is tested through the script itself, at a lock of the case's own and with no Docker.
  $build = Join-Path $PSScriptRoot 'linux-build.ps1'
  $buildLockBefore = $env:ENGINE_LINUX_BUILD_LOCK
  try {
    $p = New-LockPath
    $env:ENGINE_LINUX_BUILD_LOCK = $p
    function Start-Build([double]$seconds, [string]$log) {
      return Start-Process pwsh -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -ArgumentList @('-NoProfile', '-File', "`"$build`"", '-LockOnly', "$seconds")
    }
    Write-Lock $p 'claude-engine' 30
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $builds = @()
    foreach ($n in 1..3) {
      $builds += Start-Build (0.3 + $n / 100) "$p.build$n.log"
      if (-not (Wait-Line $p $builds.Count)) { break }
    }
    $line = Get-MachineLockLine -Path $p
    Test-That 'three builds wait in the line in the order they started, each asking ten minutes' {
      $line.Count -eq 3 -and ($line | ForEach-Object Purpose) -join '|' -match '^linux-build -LockOnly 0\.31 in .+\|linux-build -LockOnly 0\.32 in .+\|linux-build -LockOnly 0\.33 in ' -and
      @($line | Where-Object { $_.DeclaredSeconds -ne 600 }).Count -eq 0
    }
    $out = & pwsh -NoProfile -File $build -LockOnly 0.1 -NoWait 2>&1
    Test-That '-NoWait behind them exits 2 and names the holder and its place in the line' {
      $LASTEXITCODE -eq 2 -and "$out" -match "linux-build: the build lock is not this build's to take: held by 'claude-engine': held by a test \(until .+\); this ask was behind 3 of the 4 in the line, first 'claude-engine' \(pid \d+\): linux-build -LockOnly 0\.31 in "
    }
    $out = & pwsh -NoProfile -File $cli status -LockFile $p
    Test-That 'gpu-lock.ps1 status -LockFile shows the build lock''s line' { $out.Count -eq 5 -and $out[1] -eq '3 waiting, first served first:' -and $out[4] -match 'LockOnly 0\.33' }
    # Who holds it, looked at every 20 ms while the three are served: their pids, in order.
    $served = New-Object System.Collections.Generic.List[long]
    Remove-Item -LiteralPath $p -Force
    $until = [DateTime]::UtcNow.AddSeconds(120)
    while (@($builds | Where-Object { -not $_.HasExited }).Count -gt 0 -and [DateTime]::UtcNow -lt $until) {
      $held = Read-MachineLock -Path $p
      if ($held.Readable -and ($served.Count -eq 0 -or $served[$served.Count - 1] -ne $held.Pid)) { $served.Add($held.Pid) }
      Start-Sleep -Milliseconds 20
    }
    Test-That "and are served in that order (three script runs, $([int]$sw.Elapsed.TotalSeconds) s)" {
      ($served -join '|') -eq (($builds | ForEach-Object Id) -join '|') -and -not (Test-Path -LiteralPath $p)
    }
    Test-That 'a waiting build said who held the lock and where it stood' {
      (Get-Content -LiteralPath "$p.build3.log" -Raw) -match "linux-build: waiting; held by 'claude-engine': held by a test \(until .+\); this ask is behind 2 of the 3 in the line, first 'claude-engine' \(pid $($builds[0].Id)\)"
    }
  } finally {
    $env:ENGINE_LINUX_BUILD_LOCK = $buildLockBefore
  }} finally {
  if ($KeepTemp) { Write-Host "kept $scratch" } else { Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks FAILED" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0
