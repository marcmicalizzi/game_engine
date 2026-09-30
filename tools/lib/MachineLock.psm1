#requires -Version 7
<#
.SYNOPSIS
  One implementation of the machine-lock protocol, for every PowerShell tool that takes a lock.

.DESCRIPTION
  The protocol is D:\workspace\GPU-LOCK.md's, written for the shared GPU and reused unchanged for
  the Linux container build: one JSON file whose presence means "taken",

      {"owner":"claude-engine","purpose":"...","pid":1234,
       "started":"2026-09-22T18:40:00Z","expires":"2026-09-22T19:10:00Z","host":"DESKTOP"}

  created atomically (create-new), refreshed before `expires` passes, and deleted to release. An
  expired lock may be broken; an unexpired one never is; a lock owned by "marc" (a person) is never
  broken by a tool. Two files use it today:

    gpu           $env:ENGINE_GPU_LOCK,          default D:\workspace\gpu.lock
    linux-build   $env:ENGINE_LINUX_BUILD_LOCK,  default D:\workspace\linux-build.lock

  (`/tmp/<name>.lock` off Windows.) tools/gpu-lock.ps1 is the command line over this module,
  tools/linux-build.ps1 takes the build lock through it, and tools/ci/gpu-smoke.ps1 and
  tools/ci/reference-compare.ps1 take the GPU lock through Open-MachineLockSession. The C++ side —
  the bench harness's --gpu-lock, and every test process that opens a GPU device, which takes the
  lock for as long as the device lives — reads and takes it through foundation/gpu_lock
  (docs/subsystems/gpu_lock.md), because it cannot import a PowerShell module; the two are kept to
  the same file format and the same rules.

  What this adds to the rules in GPU-LOCK.md, and why:

  - **A lock is refreshed by rename, never by rewriting in place.** A reader that catches a file
    half-written sees JSON that does not parse; the first version rewrote the file with
    Set-Content, and a waiter that read it at that moment judged it unreadable, therefore expired,
    and deleted a live lock. A new body is written beside the lock and moved over it.
  - **An unreadable file is "being written", not "expired"**, until it is older than a minute. A
    truncated file left by a crash then stops blocking the machine within a minute instead of
    forever, and a lock that is simply mid-creation is never broken.
  - **Breaking is verified.** Two waiters that both judge a lock expired would otherwise both
    delete, and the second would delete the lock the first had just created. The breaker moves the
    file aside, compares what it moved with what it judged, deletes it only if they are the same
    file, and puts it back if they are not.
  - **Holders that run long keep the lease short and refresh it** (Start-MachineLockHeartbeat),
    so a holder that dies holds the machine for one lease and not for the length of a build.
  - **Release deletes only the lock this process wrote** (owner, pid and start time all match).
    Every engine agent is "claude-engine", so an owner match alone would let one agent's release
    delete another agent's lock.
#>

Set-StrictMode -Version Latest

# An unreadable lock file younger than this is assumed to be mid-write.
$script:UnreadableStaleSeconds = 60

function Get-MachineLockPath {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [ValidateSet('gpu', 'linux-build')] [string]$Kind)
  $isWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
  switch ($Kind) {
    'gpu' {
      if ($env:ENGINE_GPU_LOCK) { return $env:ENGINE_GPU_LOCK }
      if ($isWin) { return 'D:\workspace\gpu.lock' } else { return '/tmp/gpu.lock' }
    }
    'linux-build' {
      if ($env:ENGINE_LINUX_BUILD_LOCK) { return $env:ENGINE_LINUX_BUILD_LOCK }
      if ($isWin) { return 'D:\workspace\linux-build.lock' } else { return '/tmp/linux-build.lock' }
    }
  }
}

# Who takes a lock when the caller does not say: one variable for both locks, because it names a
# who and not a what.
function Get-MachineLockOwner {
  if ($env:ENGINE_GPU_LOCK_OWNER) { return $env:ENGINE_GPU_LOCK_OWNER }
  return 'claude-engine'
}

function ConvertTo-MachineLockTime([DateTime]$time) {
  return $time.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ', [Globalization.CultureInfo]::InvariantCulture)
}

# ISO 8601 back to a UTC DateTime, or $null. Accepts what PowerShell writes ("...:00Z") and what
# Python's isoformat() writes ("...:00.123456+00:00").
function ConvertFrom-MachineLockTime([string]$text) {
  if ([string]::IsNullOrWhiteSpace($text)) { return $null }
  $styles = [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal
  $parsed = [DateTime]::MinValue
  if ([DateTime]::TryParse($text, [Globalization.CultureInfo]::InvariantCulture, $styles, [ref]$parsed)) {
    return $parsed
  }
  return $null
}

<#
  Reads a lock file without waiting and without writing. The result always has the same shape:
  Present, Readable, Raw, Owner, Purpose, Pid, Started, ExpiresText, Expires (UTC DateTime or
  $null), Host, Expired, Blocking. `Blocking` is the one to act on: present and either unexpired or
  a person's.

  The JSON is read with System.Text.Json rather than ConvertFrom-Json on purpose: ConvertFrom-Json
  turns an ISO 8601 string into a DateTime, and PowerShell 7.4 (the container's) has no switch to
  stop it, so `expires` would come back in whatever form the local culture prints.
#>
function Read-MachineLock {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [string]$Path)
  $result = [ordered]@{
    Present = $false; Readable = $false; Raw = $null
    Owner = ''; Purpose = ''; Pid = 0; Started = ''; ExpiresText = ''; Expires = $null; Host = ''
    Expired = $false; Blocking = $false
  }
  $raw = $null
  $age = 0.0
  try {
    $raw = [IO.File]::ReadAllText($Path)
    $age = ([DateTime]::UtcNow - [IO.File]::GetLastWriteTimeUtc($Path)).TotalSeconds
  } catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] {
    return [pscustomobject]$result
  } catch {
    # Present but not readable right now (a writer holds it exclusively): mid-write.
    $result.Present = $true
    $result.Blocking = $true
    return [pscustomobject]$result
  }
  $result.Present = $true
  $result.Raw = $raw
  $fields = @{}
  try {
    $doc = [Text.Json.JsonDocument]::Parse($raw)
    try {
      if ($doc.RootElement.ValueKind -eq [Text.Json.JsonValueKind]::Object) {
        foreach ($p in $doc.RootElement.EnumerateObject()) {
          $fields[$p.Name] = switch ($p.Value.ValueKind) {
            'String' { $p.Value.GetString() }
            'Number' { $p.Value.GetRawText() }
            default  { $p.Value.GetRawText() }
          }
        }
        $result.Readable = $true
      }
    } finally { $doc.Dispose() }
  } catch { $result.Readable = $false }

  if ($result.Readable) {
    if ($fields.ContainsKey('owner')) { $result.Owner = [string]$fields['owner'] }
    if ($fields.ContainsKey('purpose')) { $result.Purpose = [string]$fields['purpose'] }
    if ($fields.ContainsKey('pid')) { $n = 0L; if ([long]::TryParse([string]$fields['pid'], [ref]$n)) { $result.Pid = $n } }
    if ($fields.ContainsKey('started')) { $result.Started = [string]$fields['started'] }
    if ($fields.ContainsKey('host')) { $result.Host = [string]$fields['host'] }
    if ($fields.ContainsKey('expires')) {
      $result.ExpiresText = [string]$fields['expires']
      $result.Expires = ConvertFrom-MachineLockTime $result.ExpiresText
    }
    # No expiry, or one nobody can read, is a promise nobody made: treat it as already broken.
    $result.Expired = ($null -eq $result.Expires) -or ($result.Expires -lt [DateTime]::UtcNow)
  } else {
    $result.Expired = $age -gt $script:UnreadableStaleSeconds
  }
  $result.Blocking = (-not $result.Expired) -or ($result.Owner -eq 'marc')
  return [pscustomobject]$result
}

function Format-MachineLockHolder($seen) {
  if (-not $seen.Readable) { return 'an unreadable lock file (being written, or left by a crash)' }
  $until = if ($seen.ExpiresText) { " (until $($seen.ExpiresText))" } else { '' }
  return "'$($seen.Owner)': $($seen.Purpose)$until"
}

function New-MachineLockBody($handle) {
  return ([ordered]@{
      owner = $handle.Owner; purpose = $handle.Purpose; pid = $handle.Pid
      started = $handle.Started; expires = $handle.ExpiresText; host = $handle.Host
    } | ConvertTo-Json -Compress)
}

function Test-OwnMachineLock($handle, $seen) {
  return $seen.Readable -and $seen.Owner -eq $handle.Owner -and $seen.Pid -eq $handle.Pid -and
         $seen.Started -eq $handle.Started
}

# Create-new or nothing: the one atomic step the whole protocol rests on.
function Invoke-CreateMachineLock($handle) {
  $bytes = [Text.Encoding]::UTF8.GetBytes((New-MachineLockBody $handle))
  try {
    $fs = [IO.File]::Open($handle.Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
  } catch [IO.IOException] {
    return $false
  }
  try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
  return $true
}

# Writes a new body beside the lock and moves it over. A reader sees the old file or the new one,
# never half of either. Retries briefly: on Windows a reader holding the file open without
# FILE_SHARE_DELETE makes the replace fail for the few milliseconds it reads.
function Set-MachineLockBody([string]$path, [string]$body) {
  $tmp = "$path.$PID.$([guid]::NewGuid().ToString('N')).tmp"
  [IO.File]::WriteAllText($tmp, $body)
  for ($attempt = 0; $attempt -lt 20; $attempt++) {
    try { [IO.File]::Move($tmp, $path, $true); return $true }
    catch [IO.IOException], [UnauthorizedAccessException] { Start-Sleep -Milliseconds 50 }
  }
  Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
  return $false
}

# Breaks the lock `seen` described, and only that one. Moving the file aside is atomic, so exactly
# one breaker gets any given file; if the file it got is not the one it judged (somebody broke the
# stale lock and took a fresh one in between), it puts the fresh one back.
function Remove-StaleMachineLock([string]$path, $seen) {
  if ($seen.Owner -eq 'marc') { return $false }
  $grave = "$path.stale-$PID-$([guid]::NewGuid().ToString('N'))"
  try { [IO.File]::Move($path, $grave) } catch { return $false }
  $moved = try { [IO.File]::ReadAllText($grave) } catch { $null }
  if ($moved -eq $seen.Raw) {
    Remove-Item -LiteralPath $grave -Force -ErrorAction SilentlyContinue
    return $true
  }
  try { [IO.File]::Move($grave, $path) } catch { Remove-Item -LiteralPath $grave -Force -ErrorAction SilentlyContinue }
  return $false
}

<#
  Takes the lock at -Path. Returns an object with Outcome and, when taken, Handle:

    Taken        the file is ours; release it with Exit-MachineLock
    Busy         someone else holds it and -Wait was not given (Holder says who)
    TimedOut     someone else still held it after -TimeoutMinutes (Holder says who)
    NoDirectory  the lock's directory does not exist: this machine does not take part in the
                 protocol (a CI runner, a laptop). Callers run unlocked and say so.

  While waiting it prints who holds the lock whenever that changes, so a person reading the log
  knows what the wait is for.
#>
function Enter-MachineLock {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)] [string]$Path,
    [Parameter(Mandatory)] [string]$Purpose,
    [string]$Owner = (Get-MachineLockOwner),
    [double]$LeaseMinutes = 30,
    [switch]$Wait,
    [int]$PollSeconds = 20,
    [double]$TimeoutMinutes = 240,
    [string]$Label = 'lock'
  )
  $dir = Split-Path -Parent $Path
  if ($dir -and -not (Test-Path -LiteralPath $dir -PathType Container)) {
    return [pscustomobject]@{ Outcome = 'NoDirectory'; Handle = $null; Holder = $null }
  }
  $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
  $announced = ''
  $waitStarted = [DateTime]::UtcNow
  while ($true) {
    $now = [DateTime]::UtcNow
    $handle = [pscustomobject]@{
      Path = $Path; Owner = $Owner; Purpose = $Purpose; Pid = [long]$PID
      Started = (ConvertTo-MachineLockTime $now)
      ExpiresText = (ConvertTo-MachineLockTime $now.AddMinutes($LeaseMinutes))
      LeaseMinutes = $LeaseMinutes
      Host = $(if ($env:COMPUTERNAME) { $env:COMPUTERNAME } else { [Environment]::MachineName })
    }
    if (Invoke-CreateMachineLock $handle) {
      if ($announced) {
        Write-Host ("{0}: acquired after waiting {1:N0} s" -f $Label, ([DateTime]::UtcNow - $waitStarted).TotalSeconds) -ForegroundColor Yellow
      }
      return [pscustomobject]@{ Outcome = 'Taken'; Handle = $handle; Holder = $null }
    }
    $seen = Read-MachineLock -Path $Path
    if (-not $seen.Present) { continue }   # released between our attempt and our look: try again now
    if (-not $seen.Blocking) {
      Write-Host "${Label}: breaking an expired lock held by $(Format-MachineLockHolder $seen)" -ForegroundColor Yellow
      if (Remove-StaleMachineLock $Path $seen) { continue }
      $seen = Read-MachineLock -Path $Path
      if (-not $seen.Present) { continue }
    }
    if (-not $Wait) { return [pscustomobject]@{ Outcome = 'Busy'; Handle = $null; Holder = $seen } }
    $remaining = ($deadline - [DateTime]::UtcNow).TotalSeconds
    if ($remaining -le 0) { return [pscustomobject]@{ Outcome = 'TimedOut'; Handle = $null; Holder = $seen } }
    # Announced once per holder, not once per refresh: a holder's heartbeat moves `expires` every
    # couple of minutes, and a line per move would bury the one that says somebody new has it.
    $holder = "$($seen.Owner)|$($seen.Purpose)|$($seen.Pid)|$($seen.Started)|$($seen.Readable)"
    if ($holder -ne $announced) {
      Write-Host "${Label}: waiting; held by $(Format-MachineLockHolder $seen)" -ForegroundColor Yellow
      $announced = $holder
    }
    Start-Sleep -Seconds ([Math]::Max(1, [Math]::Min($PollSeconds, [Math]::Ceiling($remaining))))
  }
}

# Pushes the expiry of a lock this process holds out by its lease. False when the file is no
# longer ours (somebody broke it after it expired, or it was deleted by hand).
function Update-MachineLock {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Handle, [double]$LeaseMinutes = 0)
  $seen = Read-MachineLock -Path $Handle.Path
  if (-not (Test-OwnMachineLock $Handle $seen)) { return $false }
  $lease = if ($LeaseMinutes -gt 0) { $LeaseMinutes } else { $Handle.LeaseMinutes }
  $Handle.ExpiresText = ConvertTo-MachineLockTime ([DateTime]::UtcNow.AddMinutes($lease))
  return (Set-MachineLockBody $Handle.Path (New-MachineLockBody $Handle))
}

# Deletes the lock if it is still the one this handle wrote. Idempotent.
function Exit-MachineLock {
  [CmdletBinding()]
  param($Handle)
  if ($null -eq $Handle) { return }
  $seen = Read-MachineLock -Path $Handle.Path
  if (Test-OwnMachineLock $Handle $seen) {
    Remove-Item -LiteralPath $Handle.Path -Force -ErrorAction SilentlyContinue
  }
}

<#
  Refreshes the lease from a background thread in this process every -IntervalSeconds, so that a
  holder doing one long thing (a container build, a test suite) keeps a short lease. The thread
  dies with the process, which is the point: a holder that is killed stops refreshing, and the
  lock expires one lease later. Stop it with Stop-MachineLockHeartbeat *before* Exit-MachineLock,
  or a refresh can land after the release and write the lock back.
#>
function Start-MachineLockHeartbeat {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Handle, [int]$IntervalSeconds = 0)
  if ($IntervalSeconds -le 0) { $IntervalSeconds = [int][Math]::Max(10, $Handle.LeaseMinutes * 60 / 5) }
  $module = Join-Path $PSScriptRoot 'MachineLock.psm1'
  return Start-ThreadJob -Name "machine-lock-heartbeat-$PID" -ArgumentList $module, $Handle, $IntervalSeconds -ScriptBlock {
    param($module, $handle, $interval)
    Import-Module $module
    while ($true) {
      Start-Sleep -Seconds $interval
      if (-not (Update-MachineLock -Handle $handle)) { break }   # not ours any more: nothing to keep alive
    }
  }
}

function Stop-MachineLockHeartbeat {
  [CmdletBinding()]
  param($Job)
  if ($null -eq $Job) { return }
  Stop-Job -Job $Job -ErrorAction SilentlyContinue
  Remove-Job -Job $Job -Force -ErrorAction SilentlyContinue
}

<#
  Holds a lock for the rest of a script, for scripts whose locked part is too long or too full of
  `exit` and `throw` to be one script block: waits for the lock (or, with -NoWait, throws if it is
  taken), starts the heartbeat, and returns a session to hand to Close-MachineLockSession in a
  finally. On a machine without the lock's directory it warns and returns a session that holds
  nothing, so the caller's code path is the same either way.

  For the GPU lock (-Kind gpu) it also sets ENGINE_GPU_LOCK_HOLDER to this process's id, and
  ENGINE_GPU_LOCK_OWNER to the owner it holds under, until the session closes. Every child started
  meanwhile inherits them, which is how a bench executable, a test, or an engine-host started
  under the lock knows the lock it finds is held on its behalf rather than by another agent: a
  bench does not warn that the GPU was spoken for, and a test's GPU device neither waits for the
  lock nor releases it (foundation/gpu_lock). The owner goes with it because "mine" is owner and
  pid together, and a lock taken with -Owner would otherwise be a stranger's to a child whose
  environment names the default one. The other way round, a session opened where the lock is
  already held on this run's behalf (inside `gpu-lock.ps1 run`, or inside another session) holds
  nothing and waits for nothing: it would be waiting for its own parent.
#>
function Open-MachineLockSession {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)] [ValidateSet('gpu', 'linux-build')] [string]$Kind,
    [Parameter(Mandatory)] [string]$Purpose,
    [string]$Path = '',
    [string]$Owner = (Get-MachineLockOwner),
    [double]$LeaseMinutes = 10,
    [switch]$NoWait,
    [double]$TimeoutMinutes = 240
  )
  if (-not $Path) { $Path = Get-MachineLockPath -Kind $Kind }
  $label = "$Kind-lock"
  $session = [pscustomobject]@{
    Kind = $Kind; Handle = $null; Heartbeat = $null
    PreviousHolder = $env:ENGINE_GPU_LOCK_HOLDER; PreviousOwner = $env:ENGINE_GPU_LOCK_OWNER
    SetHolder = $false
  }
  # Already held on this run's behalf — a session inside `gpu-lock.ps1 run`, or inside another
  # session — is used as it is, as the C++ side does (foundation/gpu_lock): waiting for it would be
  # waiting for our own parent, until it gave up.
  if ($Kind -eq 'gpu' -and $env:ENGINE_GPU_LOCK_HOLDER) {
    $held = Read-MachineLock -Path $Path
    if ($held.Present -and $held.Readable -and -not $held.Expired -and $held.Owner -eq $Owner -and
        "$($held.Pid)" -eq "$env:ENGINE_GPU_LOCK_HOLDER") {
      Write-Host "${label}: held on this run's behalf by pid $($held.Pid) ($($held.Purpose))"
      return $session
    }
  }
  $taken = Enter-MachineLock -Path $Path -Purpose $Purpose -Owner $Owner -LeaseMinutes $LeaseMinutes `
                             -Wait:(-not $NoWait) -TimeoutMinutes $TimeoutMinutes -Label $label
  switch ($taken.Outcome) {
    'NoDirectory' {
      Write-Warning "${label}: $(Split-Path -Parent $Path) does not exist, so this machine does not use the lock; running without it"
      return $session
    }
    'Busy' { throw "${label}: held by $(Format-MachineLockHolder $taken.Holder)" }
    'TimedOut' { throw "${label}: still held after $TimeoutMinutes minutes by $(Format-MachineLockHolder $taken.Holder)" }
  }
  $session.Handle = $taken.Handle
  $session.Heartbeat = Start-MachineLockHeartbeat -Handle $taken.Handle
  if ($Kind -eq 'gpu') {
    # The holder and the owner it holds under, both: a child decides "held for me" by owner and pid
    # together, so a lock taken with -Owner has to hand that owner down, or a child with the
    # default owner would not recognise its own parent's hold and would wait for it.
    $env:ENGINE_GPU_LOCK_HOLDER = "$PID"
    $env:ENGINE_GPU_LOCK_OWNER = $Owner
    $session.SetHolder = $true
  }
  return $session
}

# Idempotent, and safe on a session that holds nothing.
function Close-MachineLockSession {
  [CmdletBinding()]
  param($Session)
  if ($null -eq $Session) { return }
  if ($Session.SetHolder) {
    $env:ENGINE_GPU_LOCK_HOLDER = $Session.PreviousHolder
    $env:ENGINE_GPU_LOCK_OWNER = $Session.PreviousOwner
    $Session.SetHolder = $false
  }
  Stop-MachineLockHeartbeat -Job $Session.Heartbeat
  $Session.Heartbeat = $null
  Exit-MachineLock -Handle $Session.Handle
  $Session.Handle = $null
}

<#
  Runs -ScriptBlock holding the lock, released in a finally so an exception or Ctrl+C still
  releases: Open-MachineLockSession and Close-MachineLockSession around one block. The block runs
  in a child scope, as `&` always does; a caller that needs the block's variables afterwards uses
  the session functions instead.
#>
function Invoke-WithMachineLock {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)] [ValidateSet('gpu', 'linux-build')] [string]$Kind,
    [Parameter(Mandatory)] [string]$Purpose,
    [Parameter(Mandatory)] [scriptblock]$ScriptBlock,
    [string]$Path = '',
    [string]$Owner = (Get-MachineLockOwner),
    [double]$LeaseMinutes = 10,
    [switch]$NoWait,
    [double]$TimeoutMinutes = 240
  )
  $session = Open-MachineLockSession -Kind $Kind -Purpose $Purpose -Path $Path -Owner $Owner `
                                     -LeaseMinutes $LeaseMinutes -NoWait:$NoWait -TimeoutMinutes $TimeoutMinutes
  try {
    & $ScriptBlock
  } finally {
    Close-MachineLockSession -Session $session
  }
}

Export-ModuleMember -Function Get-MachineLockPath, Get-MachineLockOwner, Read-MachineLock,
  Format-MachineLockHolder, Enter-MachineLock, Update-MachineLock, Exit-MachineLock,
  Start-MachineLockHeartbeat, Stop-MachineLockHeartbeat, Open-MachineLockSession,
  Close-MachineLockSession, Invoke-WithMachineLock, ConvertTo-MachineLockTime,
  ConvertFrom-MachineLockTime, Set-MachineLockBody
