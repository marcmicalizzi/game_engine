#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/slot.ps1. Runs under CTest as `tools.slot`.

.DESCRIPTION
  Builds a small repository and two slot worktrees in a fresh GUID directory under the system temp
  directory (two copies of this script never share one), then drives every subcommand through
  `pwsh -File tools/slot.ps1 ... -SlotsDir <fixture>` and asserts the exit code *and* the message:
  list shows the head, branch or detached state, dirty count, build directories and owner; reset
  detaches at the commit and keeps build/, and refuses a dirty tree, a claimed slot (unless
  -Force), a running process from under the slot, and an unknown commit; claim refuses a second
  owner; release frees the slot; warm runs the slot's own tools/dev.ps1 with -Affected -Base.

      pwsh tools/slot.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
$slotScript = Join-Path $PSScriptRoot 'slot.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$base = Join-Path ([IO.Path]::GetTempPath()) "engine-slot-$([guid]::NewGuid().ToString('N'))"

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

function Invoke-Slot([string[]]$slotArgs) {
  $out = (& pwsh -NoProfile -File $slotScript @slotArgs -SlotsDir $slots *>&1 | Out-String)
  [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

function Invoke-TGit([string]$dir, [string[]]$gitArgs) {
  $o = & git -C $dir -c user.name=t -c user.email=t@example.com -c commit.gpgsign=false @gitArgs 2>&1
  if ($LASTEXITCODE -ne 0) { throw "Invoke-TGit $gitArgs failed: $o" }
  return ($o | Out-String).Trim()
}

try {
  Write-Host 'slot tests'
  $repo = Join-Path $base 'repo'
  $slots = Join-Path $base 'slots'
  New-Item -ItemType Directory -Force -Path $repo, $slots | Out-Null
  Invoke-TGit $repo @('init', '-q', '-b', 'main') | Out-Null
  Set-Content -LiteralPath (Join-Path $repo 'a.txt') 'one'
  Invoke-TGit $repo @('add', '.') | Out-Null
  Invoke-TGit $repo @('commit', '-q', '-m', 'one') | Out-Null
  $first = Invoke-TGit $repo @('rev-parse', 'HEAD')
  Set-Content -LiteralPath (Join-Path $repo 'a.txt') 'two'
  Invoke-TGit $repo @('commit', '-q', '-am', 'two') | Out-Null
  $second = Invoke-TGit $repo @('rev-parse', 'HEAD')
  foreach ($n in 1, 2) { Invoke-TGit $repo @('worktree', 'add', '-q', '--detach', (Join-Path $slots "slot-$n"), $first) | Out-Null }
  $s1 = Join-Path $slots 'slot-1'
  $s2 = Join-Path $slots 'slot-2'
  # A fake warm build in slot-1; the fixture repo has no .gitignore, so ignore build/ the way the real one does.
  New-Item -ItemType Directory -Force -Path (Join-Path $s1 'build/msvc-debug') | Out-Null
  Set-Content -LiteralPath (Join-Path $s1 'build/msvc-debug/obj.o') 'x'
  Set-Content -LiteralPath (Join-Path $repo '.git/info/exclude') '/build/'

  $r = Invoke-Slot @('list')
  Test-That 'list exits 0 and names both slots' { $r.Code -eq 0 -and $r.Out.Contains('slot-1') -and $r.Out.Contains('slot-2') }
  Test-That 'list shows a detached head by its short sha' { $r.Out.Contains("head $($first.Substring(0, 8)) (detached)") }
  Test-That 'list shows the build directory and its time' { $r.Out -match 'builds: msvc-debug \(last \d{4}-\d\d-\d\d' }
  Test-That 'list says none for a slot with no build, and free for no owner' { $r.Out.Contains('builds: none') -and $r.Out.Contains('owner: free') }

  $r = Invoke-Slot @('claim', '-Slot', '1', '-Owner', 'W91')
  Test-That 'claim writes the marker and exits 0' { $r.Code -eq 0 -and (Test-Path (Join-Path $s1 'build/slot-owner.json')) }
  Test-That 'the marker is not a change git sees' { (Invoke-TGit $s1 @('status', '--porcelain')) -eq '' }
  $r = Invoke-Slot @('claim', '-Slot', '1', '-Owner', 'S100')
  Test-That 'a second owner is refused' { $r.Code -eq 1 -and $r.Out.Contains('already claimed by W91') }
  $r = Invoke-Slot @('claim', '-Slot', '1', '-Owner', 'W91')
  Test-That 'the same owner may claim again' { $r.Code -eq 0 }
  $r = Invoke-Slot @('list')
  Test-That 'list shows the owner' { $r.Out.Contains('owner: W91 since') }

  $r = Invoke-Slot @('reset', '-Slot', '1', '-To', $second)
  Test-That 'reset refuses a claimed slot' { $r.Code -eq 1 -and $r.Out.Contains('claimed by W91') -and (Invoke-TGit $s1 @('rev-parse', 'HEAD')) -eq $first }
  $r = Invoke-Slot @('reset', '-Slot', '1', '-To', $second, '-Force')
  Test-That '-Force crosses the claim, detaches at the commit and clears the marker' {
    $r.Code -eq 0 -and (Invoke-TGit $s1 @('rev-parse', 'HEAD')) -eq $second -and -not (Test-Path (Join-Path $s1 'build/slot-owner.json'))
  }
  Test-That 'reset leaves build/ in place' { Test-Path (Join-Path $s1 'build/msvc-debug/obj.o') }

  $r = Invoke-Slot @('reset', '-Slot', '1', '-To', 'main')
  Test-That "reset -To main resolves main in the slot's repository" { $r.Code -eq 0 -and (Invoke-TGit $s1 @('rev-parse', 'HEAD')) -eq $second }
  $r = Invoke-Slot @('reset', '-Slot', '1', '-To', 'no-such-ref')
  Test-That 'an unknown commit is refused' { $r.Code -eq 1 -and $r.Out.Contains('is not a commit') }

  Set-Content -LiteralPath (Join-Path $s2 'a.txt') 'edited'
  $r = Invoke-Slot @('reset', '-Slot', '2', '-To', $second)
  Test-That 'reset refuses a dirty tree and leaves it alone' { $r.Code -eq 1 -and $r.Out.Contains('uncommitted') -and (Invoke-TGit $s2 @('rev-parse', 'HEAD')) -eq $first }
  $r = Invoke-Slot @('list')
  Test-That 'list counts the dirty file' { $r.Out.Contains('dirty 1') }
  Invoke-TGit $s2 @('checkout', '--', 'a.txt') | Out-Null

  # A process whose executable is under the slot: a copy of pwsh sleeping.
  $pwshExe = (Get-Process -Id $PID).Path
  $copy = Join-Path $s2 ('hold' + [IO.Path]::GetExtension($pwshExe))
  $proc = $null
  try {
    $shell = Split-Path -Parent $pwshExe
    $dest = Join-Path $s2 'build/pwsh'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    # The files, not the links: the Linux install carries symbolic links for its remoting client
    # (libcrypto.so.1.0.0 and its kind) that point outside the directory and dangle in a container,
    # and a wildcard Copy-Item stops at the first of them (gate b57, 2026-11-09, all three
    # containers); the copy of pwsh that sleeps here needs none of them.
    Get-ChildItem -LiteralPath $shell -Recurse -Force -File | Where-Object { -not $_.LinkType } | ForEach-Object {
      $rel = $_.FullName.Substring($shell.Length).TrimStart('\', '/')
      $to = Join-Path $dest $rel
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
      Copy-Item -LiteralPath $_.FullName -Destination $to -Force
    }
    $held = Join-Path $s2 ('build/pwsh/' + (Split-Path -Leaf $pwshExe))
    $proc = Start-Process -FilePath $held -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 60' -PassThru
    Start-Sleep -Milliseconds 800
    $r = Invoke-Slot @('reset', '-Slot', '2', '-To', $second)
    Test-That 'reset refuses while a process runs from under the slot' { $r.Code -eq 1 -and $r.Out.Contains('running process') -and (Invoke-TGit $s2 @('rev-parse', 'HEAD')) -eq $first }
  } finally {
    # The test started this one itself, so the test ends it.
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
  }
  $r = Invoke-Slot @('reset', '-Slot', '2', '-To', $second)
  Test-That 'and resets once it has ended' { $r.Code -eq 0 -and (Invoke-TGit $s2 @('rev-parse', 'HEAD')) -eq $second }

  $r = Invoke-Slot @('release', '-Slot', '1')
  Test-That 'release of a free slot says so and exits 0' { $r.Code -eq 0 -and $r.Out.Contains('was not claimed') }
  Invoke-Slot @('claim', '-Slot', '1', '-Owner', 'G136') | Out-Null
  $r = Invoke-Slot @('release', '-Slot', '1')
  Test-That 'release removes the marker' { $r.Code -eq 0 -and $r.Out.Contains('was G136') -and -not (Test-Path (Join-Path $s1 'build/slot-owner.json')) }

  $r = Invoke-Slot @('claim', '-Slot', '1')
  Test-That 'claim without -Owner is a usage error (2)' { $r.Code -eq 2 }
  $r = Invoke-Slot @('reset')
  Test-That 'reset without -Slot is a usage error (2)' { $r.Code -eq 2 }
  $r = Invoke-Slot @('reset', '-Slot', '9')
  Test-That 'a slot that does not exist is refused' { $r.Code -eq 1 -and $r.Out.Contains('slot-9 is not a worktree') }

  # warm runs the slot's own tools/dev.ps1: a stub that records its arguments.
  New-Item -ItemType Directory -Force -Path (Join-Path $s1 'tools') | Out-Null
  Set-Content -LiteralPath (Join-Path $s1 'tools/dev.ps1') 'param([string]$Command,[string]$Preset,[switch]$Affected,[string]$Base); "stub $Command $Preset $Affected $Base" | Add-Content (Join-Path $PSScriptRoot "../warm.log")'
  $r = Invoke-Slot @('warm', '-Slot', '1', '-Preset', 'msvc-debug,msvc-release', '-Base', 'abc1234')
  $log = Get-Content -LiteralPath (Join-Path $s1 'warm.log') -ErrorAction SilentlyContinue
  Test-That 'warm builds each preset in the slot with -Affected -Base' {
    $r.Code -eq 0 -and $log.Count -eq 2 -and $log[0] -eq 'stub build msvc-debug True abc1234' -and $log[1] -eq 'stub build msvc-release True abc1234'
  }
} finally {
  if ($KeepTemp) {
    Write-Host "kept $base"
  } else {
    Remove-Item -Recurse -Force -LiteralPath $base -ErrorAction SilentlyContinue
  }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "slot: $checks checks passed" -ForegroundColor Green
exit 0
