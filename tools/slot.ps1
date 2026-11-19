#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The warm worktree pool: slot worktrees whose builds stay in place between agents (roadmap T75).

.DESCRIPTION
  tools/slot.ps1 <command> [-Slot N] [options]

  A slot is a worktree at <main checkout>/.claude/worktrees/slot-N with its own build/<preset>
  directories, kept warm. The coordinator resets a slot to main after each merge and assigns one to
  each heavy agent by brief; the agent works in it on a branch of its own and leaves its harness
  worktree alone (docs/ci/what-to-run.md, "The slot pool"). A slot has no branch of its own: it is
  always detached between agents, and the agent makes its branch.

  Commands:
    list      Each slot's path, HEAD, branch or detached, dirty count, the build directories that
              exist with the time of the newest file under each (its last use), its size and file
              count, and the owner marker; then the pool's total. A warm msvc-debug tree is about
              21 GB in 20,000 files, so the sizes are what the pool costs the drive it lives on.
    add       -Slot N [-To <sha|main>]. Makes slot-N: `git worktree add --detach` at the commit
              (default main) in the slots directory. Refuses a slot that exists. The new slot has
              no build/: its first build is whole, from the compiler cache (warm it).
    reset     -Slot N [-To <sha|main>] [-Force]. Detaches the slot at the commit (default main).
              Refuses a dirty tree, a slot with a process whose executable is under it, and a
              claimed slot unless -Force. Leaves build/ in place.
    claim     -Slot N -Owner <row>. Writes build/slot-owner.json (git-ignored with build/): the
              owner, the time, the branch. Refuses a slot already claimed by someone else.
    release   -Slot N. Removes the marker.
    warm      -Slot N [-Preset <names>] [-Base <sha>]. tools/dev.ps1 build in the slot for each
              preset (default msvc-debug,msvc-release), -Affected -Base <sha> when -Base is
              given. Honours CMAKE_BUILD_PARALLEL_LEVEL.

  -SlotsDir overrides where the slot-N directories are looked for (the tests use it); the default
  is <main checkout>/.claude/worktrees, found through the git common directory so the script works
  from any worktree. -Repo names the repository `add` makes a worktree of (the tests use it); the
  default is that main checkout.

  Who runs it: the coordinator claims a slot for a row before the row's agent starts, and resets it
  after the merge; the agent works in the slot and never runs this script (docs/ci/what-to-run.md,
  "The slot pool").

  Exit codes: 0 done, 1 refused or failed (the reason is printed), 2 usage error.
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0)]
  [ValidateSet('list', 'add', 'reset', 'claim', 'release', 'warm')]
  [string]$Command = 'list',
  [int]$Slot = 0,
  [string]$To = 'main',
  [string]$Owner,
  [string[]]$Preset = @('msvc-debug', 'msvc-release'),
  [string]$Base,
  [string]$SlotsDir,
  [string]$Repo,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = $PSScriptRoot

function Stop-Usage([string]$message) {
  Write-Host "slot: $message" -ForegroundColor Red
  exit 2
}

function Stop-Refused([string]$message) {
  Write-Host "slot: refused: $message" -ForegroundColor Red
  exit 1
}

function Get-MainRoot {
  if ($Repo) { return [System.IO.Path]::GetFullPath($Repo) }
  $common = & git -C $ScriptRoot rev-parse --path-format=absolute --git-common-dir 2>$null
  if (-not $common) { Stop-Usage 'not inside a git checkout' }
  return Split-Path -Parent ([System.IO.Path]::GetFullPath($common))
}

function Get-SlotsDir {
  if ($SlotsDir) { return [System.IO.Path]::GetFullPath($SlotsDir) }
  return Join-Path (Get-MainRoot) '.claude/worktrees'
}

# A size as list prints it: MB under a gigabyte, GB above (10^6 and 10^9 bytes).
function Format-Size([long]$bytes) {
  if ($bytes -lt 1e9) { return ('{0:N0} MB' -f ($bytes / 1e6)) }
  return ('{0:N1} GB' -f ($bytes / 1e9))
}

# One pass over a build tree: its size, its file count and its newest write (the tree's last use).
# DirectoryInfo's enumeration carries each file's length and time, so this costs one directory walk
# and no stat per file: a few seconds for a 20,000-file tree.
function Measure-BuildTree([string]$path) {
  $bytes = [long]0; $files = 0; $newest = [datetime]::MinValue
  foreach ($f in ([System.IO.DirectoryInfo]::new($path)).EnumerateFiles('*', [System.IO.SearchOption]::AllDirectories)) {
    $bytes += $f.Length; $files++
    if ($f.LastWriteTime -gt $newest) { $newest = $f.LastWriteTime }
  }
  [pscustomobject]@{ Bytes = $bytes; Files = $files; Newest = if ($files -gt 0) { $newest } else { $null } }
}

function Get-SlotPath([int]$n) {
  if ($n -lt 1) { Stop-Usage '-Slot N is required (1 or more)' }
  $path = Join-Path (Get-SlotsDir) "slot-$n"
  if (-not (Test-Path -LiteralPath (Join-Path $path '.git'))) { Stop-Refused "slot-$n is not a worktree at $path" }
  return $path
}

function Invoke-Git([string]$path, [string[]]$gitArgs) {
  $out = & git -C $path @gitArgs 2>&1
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = @($out | ForEach-Object { "$_" }) }
}

function Get-MarkerPath([string]$path) { Join-Path $path 'build/slot-owner.json' }

function Read-Marker([string]$path) {
  $m = Get-MarkerPath $path
  if (-not (Test-Path -LiteralPath $m)) { return $null }
  try { return Get-Content -Raw -LiteralPath $m | ConvertFrom-Json } catch { return [pscustomobject]@{ owner = '(unreadable marker)'; time = ''; branch = '' } }
}

# Best effort: a process whose executable lives under the slot. A shell sitting in the directory
# has no file open that Windows shows here, so this finds compilers, test runs and engine binaries,
# which are what a reset would break (LNK1168 for the next build).
function Get-ProcessesUnder([string]$path) {
  $prefix = ([System.IO.Path]::GetFullPath($path)).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
  @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) })
}

function Get-SlotDirs {
  $dir = Get-SlotsDir
  if (-not (Test-Path -LiteralPath $dir)) { return @() }
  @(Get-ChildItem -LiteralPath $dir -Directory -Filter 'slot-*' |
      Where-Object { $_.Name -match '^slot-(\d+)$' -and (Test-Path -LiteralPath (Join-Path $_.FullName '.git')) } |
      Sort-Object { [int]($_.Name -replace '^slot-', '') })
}

function Invoke-List {
  $slots = Get-SlotDirs
  if ($slots.Count -eq 0) { Write-Host "slot: no slot worktrees under $(Get-SlotsDir)"; return }
  $poolBytes = [long]0; $poolFiles = 0
  foreach ($s in $slots) {
    $head = (Invoke-Git $s.FullName @('rev-parse', '--short=8', 'HEAD')).Out | Select-Object -First 1
    $branch = (Invoke-Git $s.FullName @('symbolic-ref', '--short', '-q', 'HEAD')).Out | Select-Object -First 1
    $dirty = @((Invoke-Git $s.FullName @('status', '--porcelain')).Out | Where-Object { $_ }).Count
    $state = if ($branch) { "branch $branch" } else { 'detached' }
    Write-Host "$($s.Name)  $($s.FullName)"
    Write-Host "  head $head ($state), dirty $dirty"
    $buildRoot = Join-Path $s.FullName 'build'
    $builds = @()
    if (Test-Path -LiteralPath $buildRoot) {
      foreach ($b in Get-ChildItem -LiteralPath $buildRoot -Directory) {
        $t = Measure-BuildTree $b.FullName
        $poolBytes += $t.Bytes; $poolFiles += $t.Files
        $when = if ($t.Newest) { $t.Newest.ToString('yyyy-MM-dd HH:mm') } else { 'empty' }
        $builds += "$($b.Name) (last $when, $(Format-Size $t.Bytes), $($t.Files) files)"
      }
    }
    Write-Host "  builds: $(if ($builds.Count) { $builds -join ', ' } else { 'none' })"
    $m = Read-Marker $s.FullName
    if ($m) { Write-Host "  owner: $($m.owner) since $($m.time) on $($m.branch)" } else { Write-Host '  owner: free' }
  }
  Write-Host "pool: $($slots.Count) slots, build trees $(Format-Size $poolBytes) in $poolFiles files"
}

function Invoke-Add {
  if ($Slot -lt 1) { Stop-Usage '-Slot N is required (1 or more)' }
  $dir = Get-SlotsDir
  $path = Join-Path $dir "slot-$Slot"
  if (Test-Path -LiteralPath $path) { Stop-Refused "slot-$Slot already exists at $path" }
  $main = Get-MainRoot
  $rev = Invoke-Git $main @('rev-parse', '--verify', '--quiet', "$To^{commit}")
  if ($rev.Code -ne 0 -or -not $rev.Out[0]) { Stop-Refused "'$To' is not a commit" }
  $sha = $rev.Out[0]
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $add = Invoke-Git $main @('worktree', 'add', '--detach', $path, $sha)
  if ($add.Code -ne 0) { Stop-Refused "git worktree add failed: $($add.Out -join ' ')" }
  Write-Host "slot-$Slot added at $path, detached at $($sha.Substring(0, 8)) ($To); no build yet: its first is whole, from the compiler cache (slot.ps1 warm -Slot $Slot)"
}

function Invoke-Reset {
  $path = Get-SlotPath $Slot
  $m = Read-Marker $path
  if ($m -and -not $Force) { Stop-Refused "slot-$Slot is claimed by $($m.owner) since $($m.time); release it first, or -Force" }
  $dirty = @((Invoke-Git $path @('status', '--porcelain')).Out | Where-Object { $_ })
  if ($dirty.Count -gt 0) { Stop-Refused "slot-$Slot has $($dirty.Count) uncommitted change(s), first: $($dirty[0])" }
  $busy = Get-ProcessesUnder $path
  if ($busy.Count -gt 0) {
    $names = ($busy | Select-Object -First 5 | ForEach-Object { "$($_.ProcessName) ($($_.Id))" }) -join ', '
    Stop-Refused "slot-$Slot has $($busy.Count) running process(es) from under it: $names"
  }
  $rev = Invoke-Git $path @('rev-parse', '--verify', '--quiet', "$To^{commit}")
  if ($rev.Code -ne 0 -or -not $rev.Out[0]) { Stop-Refused "'$To' is not a commit" }
  $sha = $rev.Out[0]
  $co = Invoke-Git $path @('checkout', '--detach', $sha)
  if ($co.Code -ne 0) { Stop-Refused "git checkout --detach failed: $($co.Out -join ' ')" }
  if ($m) { Remove-Item -LiteralPath (Get-MarkerPath $path) -Force }
  Write-Host "slot-$Slot reset: detached at $($sha.Substring(0, 8)) ($To); build/ left in place"
}

function Invoke-Claim {
  if (-not $Owner) { Stop-Usage 'claim needs -Owner <row>' }
  $path = Get-SlotPath $Slot
  $m = Read-Marker $path
  if ($m -and $m.owner -ne $Owner) { Stop-Refused "slot-$Slot is already claimed by $($m.owner) since $($m.time)" }
  $branch = (Invoke-Git $path @('symbolic-ref', '--short', '-q', 'HEAD')).Out | Select-Object -First 1
  $dir = Join-Path $path 'build'
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $marker = [ordered]@{ owner = $Owner; time = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'); branch = if ($branch) { "$branch" } else { '(detached)' } }
  [System.IO.File]::WriteAllText((Get-MarkerPath $path), ($marker | ConvertTo-Json), (New-Object System.Text.UTF8Encoding $false))
  Write-Host "slot-$Slot claimed by $Owner"
}

function Invoke-Release {
  $path = Get-SlotPath $Slot
  $m = Read-Marker $path
  if (-not $m) { Write-Host "slot-$Slot was not claimed"; return }
  Remove-Item -LiteralPath (Get-MarkerPath $path) -Force
  Write-Host "slot-$Slot released (was $($m.owner))"
}

function Invoke-Warm {
  $path = Get-SlotPath $Slot
  $dev = Join-Path $path 'tools/dev.ps1'
  if (-not (Test-Path -LiteralPath $dev)) { Stop-Refused "slot-$Slot has no tools/dev.ps1" }
  $names = @($Preset | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
  foreach ($p in $names) {
    $a = @('-NoProfile', '-File', $dev, 'build', '-Preset', $p)
    if ($Base) { $a += @('-Affected', '-Base', $Base) }
    Write-Host "slot-$Slot warm: dev.ps1 build -Preset $p$(if ($Base) { " -Affected -Base $Base" }) (CMAKE_BUILD_PARALLEL_LEVEL=$(if ($env:CMAKE_BUILD_PARALLEL_LEVEL) { $env:CMAKE_BUILD_PARALLEL_LEVEL } else { 'unset' }))"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $path
    try { & pwsh @a; $code = $LASTEXITCODE } finally { Pop-Location }
    if ($code -ne 0) { Stop-Refused "build of $p failed with exit $code after $([int]$sw.Elapsed.TotalSeconds) s" }
    Write-Host "slot-$Slot warm: $p built in $([int]$sw.Elapsed.TotalSeconds) s"
  }
}

switch ($Command) {
  'list'    { Invoke-List }
  'add'     { Invoke-Add }
  'reset'   { Invoke-Reset }
  'claim'   { Invoke-Claim }
  'release' { Invoke-Release }
  'warm'    { Invoke-Warm }
}
exit 0
