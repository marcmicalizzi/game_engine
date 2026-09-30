<#
.SYNOPSIS
  The machine-wide GPU lock: one holder at a time for anything heavy on the shared GPU.

.DESCRIPTION
  tools/gpu-lock.ps1 acquire -Purpose <text> [-Minutes 30] [-Owner claude-engine]
  tools/gpu-lock.ps1 release
  tools/gpu-lock.ps1 status
  tools/gpu-lock.ps1 wait    [-Purpose <text>] [-Minutes 30] [-PollSeconds 20] [-TimeoutMinutes 240]
  tools/gpu-lock.ps1 refresh [-Minutes 30]
  tools/gpu-lock.ps1 run -Purpose <text> [-Minutes 10] -Exec "<command line>"
  ... [-LockFile <path>]

  The protocol (D:\workspace\GPU-LOCK.md, also docs/subsystems/bench.md "The GPU lock") is shared
  with other agents on this machine, so the file and its rules are theirs as much as ours: a JSON
  file at $env:ENGINE_GPU_LOCK (default D:\workspace\gpu.lock), created atomically, carrying an
  owner, a purpose and an expiry. An expired lock may be broken; an unexpired one never is; a lock
  owned by "marc" is never broken by a tool. The implementation is tools/lib/MachineLock.psm1,
  which tools/linux-build.ps1 uses for its build lock too; -LockFile points this command line at
  any lock of the same format (`status -LockFile D:\workspace\linux-build.lock` says who is
  building).

  `acquire` exits 0 when the lock is yours and 2 when someone else holds it (without waiting);
  `wait` waits for it; `run` waits, runs the command with the lock refreshed in the background
  (so -Minutes is the lease a crash would leave behind, not a limit on the command), and releases
  in a finally block so a failing command still releases. Exit code of `run` is the command's.

  `run` sets ENGINE_GPU_LOCK_HOLDER to its own process id for the command, which is how a bench
  executable inside it (`--require-quiet`, `--gpu-lock`) knows the lock it finds is held on its
  behalf. A lock taken with a separate `acquire` belongs to a process that has already exited,
  and a benchmark cannot tell it from another agent's: prefer `run`, or the harness's own
  `--gpu-lock`.

  A test run does not need `run`: every test process takes the lock itself for as long as it has
  a GPU device open (docs/subsystems/gpu_lock.md), so wrapping `tools/dev.ps1 test` only holds
  the GPU through the suite's CPU-only tests as well. It still works — the tests inside find the
  lock held on their behalf and neither wait nor release — for anyone who does it anyway.

  `release` and `refresh` act on a lock whose owner is -Owner, whichever process took it, because
  `acquire` and `release` are separate processes by design. Every engine agent is
  "claude-engine", so they are for a lock you know is yours.
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0, Mandatory)] [ValidateSet('acquire', 'release', 'status', 'wait', 'refresh', 'run')] [string]$Action,
  [string]$Purpose = '',
  [int]$Minutes = 0,
  [string]$Owner = '',
  [int]$PollSeconds = 20,
  [int]$TimeoutMinutes = 240,
  [string]$Exec = '',
  [string]$LockFile = ''
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force

$Lock = if ($LockFile) { $LockFile } else { Get-MachineLockPath -Kind gpu }
if (-not $Owner) { $Owner = Get-MachineLockOwner }
# `run` refreshes its lease while the command runs, so its lease only bounds what a crash leaves
# behind and can be short; the one-shot actions cannot refresh and keep the protocol's default.
if ($Minutes -le 0) { $Minutes = if ($Action -eq 'run') { 10 } else { 30 } }

switch ($Action) {
  'status' {
    $held = Read-MachineLock -Path $Lock
    if (-not $held.Present) { Write-Output 'free'; exit 0 }
    $state = if ($held.Blocking) { 'held' } else { 'EXPIRED' }
    Write-Output "$state by $(Format-MachineLockHolder $held) (pid $($held.Pid), since $($held.Started))"
    exit $(if ($held.Blocking) { 2 } else { 0 })
  }
  'acquire' {
    if (-not $Purpose) { throw 'acquire needs -Purpose' }
    $r = Enter-MachineLock -Path $Lock -Purpose $Purpose -Owner $Owner -LeaseMinutes $Minutes -Label 'gpu-lock'
    switch ($r.Outcome) {
      'Taken' { Write-Output "acquired ($Owner, $Minutes min)"; exit 0 }
      'NoDirectory' { throw "$(Split-Path -Parent $Lock) does not exist; this machine does not use the lock" }
      default { Write-Output "busy: held by $(Format-MachineLockHolder $r.Holder)"; exit 2 }
    }
  }
  'wait' {
    if (-not $Purpose) { throw 'wait needs -Purpose' }
    $r = Enter-MachineLock -Path $Lock -Purpose $Purpose -Owner $Owner -LeaseMinutes $Minutes -Wait `
                           -PollSeconds $PollSeconds -TimeoutMinutes $TimeoutMinutes -Label 'gpu-lock'
    switch ($r.Outcome) {
      'Taken' { Write-Output "acquired ($Owner, $Minutes min)"; exit 0 }
      'NoDirectory' { throw "$(Split-Path -Parent $Lock) does not exist; this machine does not use the lock" }
      default { Write-Output "gave up after $TimeoutMinutes minutes"; exit 3 }
    }
  }
  'refresh' {
    $held = Read-MachineLock -Path $Lock
    if (-not $held.Present) { throw 'no lock to refresh' }
    if ($held.Owner -ne $Owner) { throw "lock is held by '$($held.Owner)', not '$Owner'" }
    $handle = [pscustomobject]@{
      Path = $Lock; Owner = $held.Owner; Purpose = $held.Purpose; Pid = $held.Pid
      Started = $held.Started; ExpiresText = ''; LeaseMinutes = $Minutes; Host = $held.Host
    }
    if (-not (Update-MachineLock -Handle $handle)) { throw 'the lock changed while it was being refreshed' }
    Write-Output "refreshed until $($handle.ExpiresText)"; exit 0
  }
  'release' {
    $held = Read-MachineLock -Path $Lock
    if (-not $held.Present) { Write-Output 'already free'; exit 0 }
    if ($held.Owner -ne $Owner -and $held.Blocking) { Write-Output "not yours: held by '$($held.Owner)'"; exit 2 }
    Remove-Item -LiteralPath $Lock -Force; Write-Output 'released'; exit 0
  }
  'run' {
    if (-not $Purpose) { throw 'run needs -Purpose' }
    if (-not $Exec) { throw 'run needs -Exec "<command line>"' }
    $code = $null
    try {
      Invoke-WithMachineLock -Kind gpu -Path $Lock -Purpose $Purpose -Owner $Owner -LeaseMinutes $Minutes `
                             -TimeoutMinutes $TimeoutMinutes -ScriptBlock {
        & pwsh -NoProfile -Command $Exec
        $script:code = $LASTEXITCODE
      }
    } catch {
      Write-Output "$($_.Exception.Message)"
      exit 3
    }
    exit $(if ($null -eq $script:code) { 0 } else { $script:code })
  }
}
