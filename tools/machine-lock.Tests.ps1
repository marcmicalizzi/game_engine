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
} finally {
  if ($KeepTemp) { Write-Host "kept $scratch" } else { Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks FAILED" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0
