<#
.SYNOPSIS
  The machine-wide GPU lock: one holder at a time for anything heavy on the shared GPU.

.DESCRIPTION
  tools/gpu-lock.ps1 acquire -Purpose <text> [-Minutes 30] [-Owner claude-engine]
  tools/gpu-lock.ps1 release
  tools/gpu-lock.ps1 status
  tools/gpu-lock.ps1 wait    [-Purpose <text>] [-Minutes 30] [-PollSeconds 20] [-TimeoutMinutes 240]
  tools/gpu-lock.ps1 refresh [-Minutes 30]
  tools/gpu-lock.ps1 run -Purpose <text> [-Minutes 30] -Exec "<command line>"

  The protocol (D:\workspace\GPU-LOCK.md, also docs/subsystems/bench.md "Measuring on a shared
  machine") is shared with other agents on this machine, so the file and its rules are theirs as
  much as ours: a JSON file at $env:ENGINE_GPU_LOCK (default D:\workspace\gpu.lock), created
  atomically, carrying an owner, a purpose and an expiry. An expired lock may be broken; an
  unexpired one never is; a lock owned by "marc" is never broken by a tool.

  `acquire` exits 0 when the lock is yours and 2 when someone else holds it (without waiting);
  `wait` waits for it; `run` waits, runs the command, and releases in a finally block so a
  failing command still releases. Exit code of `run` is the command's.
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0, Mandatory)] [ValidateSet('acquire', 'release', 'status', 'wait', 'refresh', 'run')] [string]$Action,
  [string]$Purpose = '',
  [int]$Minutes = 30,
  [string]$Owner = $(if ($env:ENGINE_GPU_LOCK_OWNER) { $env:ENGINE_GPU_LOCK_OWNER } else { 'claude-engine' }),
  [int]$PollSeconds = 20,
  [int]$TimeoutMinutes = 240,
  [string]$Exec = ''
)
$ErrorActionPreference = 'Stop'
$Lock = if ($env:ENGINE_GPU_LOCK) { $env:ENGINE_GPU_LOCK } else { 'D:\workspace\gpu.lock' }

function Read-Lock {
  try { Get-Content -Raw -LiteralPath $Lock | ConvertFrom-Json } catch { $null }
}
function Now-Utc { [DateTime]::UtcNow }
function Iso([DateTime]$t) { $t.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
function Expired($held) {
  if (-not $held -or -not $held.expires) { return $true }
  try { return ([DateTime]::Parse($held.expires, $null, 'AdjustToUniversal') -lt (Now-Utc)) } catch { return $true }
}
function Try-Acquire([string]$purpose, [int]$minutes) {
  $body = [ordered]@{
    owner = $Owner; purpose = $purpose; pid = $PID
    started = Iso (Now-Utc); expires = Iso ((Now-Utc).AddMinutes($minutes)); host = $env:COMPUTERNAME
  } | ConvertTo-Json -Compress
  try {
    $fs = [IO.File]::Open($Lock, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $bytes = [Text.Encoding]::UTF8.GetBytes($body); $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
    return $true
  } catch [IO.IOException] { return $false }
}
function Break-IfStale {
  $held = Read-Lock
  if ($held -and $held.owner -eq 'marc') { return $false }   # a person's lock is never broken by a tool
  if (Expired $held) {
    Write-Host "gpu-lock: breaking an expired lock held by '$($held.owner)' ($($held.purpose))" -ForegroundColor Yellow
    Remove-Item -LiteralPath $Lock -Force -ErrorAction SilentlyContinue
    return $true
  }
  return $false
}
function Wait-Acquire([string]$purpose, [int]$minutes) {
  $deadline = (Now-Utc).AddMinutes($TimeoutMinutes)
  $announced = ''
  while ((Now-Utc) -lt $deadline) {
    if (Try-Acquire $purpose $minutes) { return $true }
    if (Break-IfStale) { continue }
    $held = Read-Lock
    $line = "gpu-lock: GPU held by '$($held.owner)': $($held.purpose) (until $($held.expires))"
    if ($line -ne $announced) { Write-Host $line -ForegroundColor Yellow; $announced = $line }
    Start-Sleep -Seconds $PollSeconds
  }
  return $false
}

switch ($Action) {
  'status' {
    if (-not (Test-Path -LiteralPath $Lock)) { Write-Output "free"; exit 0 }
    $held = Read-Lock
    $state = if (Expired $held) { 'EXPIRED' } else { 'held' }
    Write-Output "$state by '$($held.owner)': $($held.purpose) (pid $($held.pid), since $($held.started), until $($held.expires))"
    exit $(if ($state -eq 'held') { 2 } else { 0 })
  }
  'acquire' {
    if (-not $Purpose) { throw 'acquire needs -Purpose' }
    if (Try-Acquire $Purpose $Minutes) { Write-Output "acquired ($Owner, $Minutes min)"; exit 0 }
    if ((Break-IfStale) -and (Try-Acquire $Purpose $Minutes)) { Write-Output "acquired after breaking a stale lock"; exit 0 }
    $held = Read-Lock; Write-Output "busy: held by '$($held.owner)': $($held.purpose) (until $($held.expires))"; exit 2
  }
  'wait' {
    if (-not $Purpose) { throw 'wait needs -Purpose' }
    if (Wait-Acquire $Purpose $Minutes) { Write-Output "acquired ($Owner, $Minutes min)"; exit 0 }
    Write-Output "gave up after $TimeoutMinutes minutes"; exit 3
  }
  'refresh' {
    $held = Read-Lock
    if (-not $held) { throw 'no lock to refresh' }
    if ($held.owner -ne $Owner) { throw "lock is held by '$($held.owner)', not '$Owner'" }
    $held.expires = Iso ((Now-Utc).AddMinutes($Minutes))
    $held | ConvertTo-Json -Compress | Set-Content -LiteralPath $Lock -Encoding utf8 -NoNewline
    Write-Output "refreshed until $($held.expires)"; exit 0
  }
  'release' {
    $held = Read-Lock
    if (-not $held) { Write-Output "already free"; exit 0 }
    if ($held.owner -ne $Owner -and -not (Expired $held)) { Write-Output "not yours: held by '$($held.owner)'"; exit 2 }
    Remove-Item -LiteralPath $Lock -Force; Write-Output "released"; exit 0
  }
  'run' {
    if (-not $Purpose) { throw 'run needs -Purpose' }
    if (-not $Exec) { throw 'run needs -Exec "<command line>"' }
    if (-not (Wait-Acquire $Purpose $Minutes)) { Write-Output "gave up after $TimeoutMinutes minutes"; exit 3 }
    try {
      & pwsh -NoProfile -Command $Exec
      $code = $LASTEXITCODE
    } finally {
      Remove-Item -LiteralPath $Lock -Force -ErrorAction SilentlyContinue
    }
    exit $(if ($null -eq $code) { 0 } else { $code })
  }
}
