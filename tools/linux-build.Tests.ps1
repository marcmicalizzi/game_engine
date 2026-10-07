#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for the Linux build's volume sweep: tools/lib/LinuxVolumes.psm1 and `tools/linux-build.ps1
  -Prune [-Stale]` over it. Runs under CTest as `tools.linux_build`, and under Invoke-Pester.

.DESCRIPTION
  Which volumes a sweep removes is a pure function of the volumes (names, labels, sizes), the
  repository's checkouts (present, main, locked, merged, dirty) and the containers holding volumes,
  so every case writes a machine down and checks the verdicts. **Nothing here talks to a Docker
  daemon**: the two cases that run the script stop before docker is asked anything, one at argument
  validation and one with DOCKER_HOST pointed at a port nobody listens on.

      pwsh tools/linux-build.Tests.ps1
      Invoke-Pester tools/linux-build.Tests.ps1

  The same checks run either way: under Pester each one is an It, otherwise each prints a line and
  the exit status says whether all passed.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/LinuxVolumes.psm1') -Force
$buildScript = Join-Path $PSScriptRoot 'linux-build.ps1'

# Results are collected through the caller's scope rather than $script:, so the file behaves the
# same whether pwsh runs it or Pester dot-sources it.
$results = New-Object System.Collections.Generic.List[object]
function Test-That([string]$what, [scriptblock]$condition) {
  $ok = $false
  $detail = $null
  try { $ok = [bool](& $condition) } catch { $detail = $_.Exception.Message }
  $results.Add([pscustomobject]@{ What = $what; Ok = $ok; Detail = $detail })
}

function Find-Verdict([object[]]$verdicts, [string]$name) { @($verdicts | Where-Object { $_.Name -eq $name })[0] }

# --- a machine, written down -----------------------------------------------------------------------
#
# The main checkout and its worktrees, one for each rule. Roots are spelt the way linux-build.ps1's
# $Root is on Windows; the decision is pure string work, so the cases hold on Linux too.

$main = 'D:\work\engine'
function Get-Worktree([string]$name) { "D:\work\engine\.claude\worktrees\$name" }

function New-Vol([string]$role, [string]$root, [switch]$Labelled, [string]$Size = '1GB') {
  $labels = if ($Labelled) { Get-VolumeLabels -Role $role -Root $root } else { $null }
  return New-VolumeFact -Name (Get-CheckoutVolumeName -Role $role -Root $root) -Labels $labels -Size $Size
}

$roots = @{
  merged = Get-Worktree 'agent-merged'; unmerged = Get-Worktree 'agent-unmerged'
  locked = Get-Worktree 'agent-locked'; dirty = Get-Worktree 'agent-dirty'
  prunable = Get-Worktree 'agent-prunable'; held = Get-Worktree 'agent-held'
  detached = Get-Worktree 'agent-detached'; current = Get-Worktree 'agent-current'
  vanished = Get-Worktree 'agent-removed-and-pruned'; foreign = 'E:\elsewhere\engine-clone'
  slashes = Get-Worktree 'agent-git-spelling'; rebased = Get-Worktree 'agent-rebased'
}

$checkouts = @(
  (New-CheckoutFact -Root $main -IsMain -Branch 'main' -Head 'aaaa1111'),
  (New-CheckoutFact -Root $roots.merged -Branch 'worktree-agent-merged' -Merged $true),
  (New-CheckoutFact -Root $roots.unmerged -Branch 'worktree-agent-unmerged' -Merged $false),
  (New-CheckoutFact -Root $roots.locked -Branch 'worktree-agent-locked' -Merged $true -Locked $true -LockReason 'claude agent agent-locked (pid 1)'),
  (New-CheckoutFact -Root $roots.dirty -Branch 'worktree-agent-dirty' -Merged $true -Dirty $true),
  (New-CheckoutFact -Root $roots.prunable -Branch 'worktree-agent-prunable' -Present $false),
  (New-CheckoutFact -Root $roots.held -Branch 'worktree-agent-held' -Merged $true),
  (New-CheckoutFact -Root $roots.detached -Head '0123456789abcdef' -Merged $true),
  (New-CheckoutFact -Root $roots.current -Branch 'worktree-agent-current' -Merged $true),
  (New-CheckoutFact -Root $roots.rebased -Branch 'worktree-agent-rebased' -Merged $true -MergedByPatch $true),
  # git's own spelling, forward slashes: an unlabelled volume must still be found by its id.
  (New-CheckoutFact -Root ($roots.slashes -replace '\\', '/') -Branch 'worktree-agent-git-spelling' -Merged $false),
  # Roots only a label names: one gone from disk, one another clone's live checkout.
  (New-CheckoutFact -Root $roots.vanished -Listed $false -Present $false),
  (New-CheckoutFact -Root $roots.foreign -Listed $false -Present $true)
)

$orphanId = Get-ShortHash 'd:\work\engine\.claude\worktrees\agent-long-gone'
$volumes = @(
  (New-Vol src $main), (New-Vol deps $main),
  (New-Vol src $roots.merged -Labelled -Size '2.3GB'), (New-Vol deps $roots.merged -Labelled -Size '966MB'),
  (New-Vol src $roots.unmerged -Labelled), (New-Vol src $roots.locked -Labelled), (New-Vol src $roots.dirty),
  (New-Vol src $roots.prunable), (New-Vol src $roots.held -Labelled), (New-Vol src $roots.detached),
  (New-Vol src $roots.current -Labelled), (New-Vol src $roots.slashes), (New-Vol deps $roots.vanished -Labelled -Size '1.608GB'),
  (New-Vol src $roots.foreign -Labelled),
  (New-Vol src $roots.rebased -Labelled),
  (New-VolumeFact -Name "engine-linux-src-$orphanId" -Size '3.1GB'),
  (New-VolumeFact -Name "engine-linux-deps-$orphanId" -Size '1.2GB'),
  # Never anything a sweep may list: the shared cache (labelled and not), buildx's state, another
  # project's build volume, and a name that only starts like ours.
  (New-VolumeFact -Name 'engine-linux-fetch-cache' -Labels (Get-VolumeLabels -Role fetch-cache) -Size '936.8MB'),
  (New-VolumeFact -Name 'engine-linux-fetch-cache' -Size '936.8MB'),
  (New-VolumeFact -Name 'buildx_buildkit_desktop-linux_state' -Size '12GB'),
  (New-VolumeFact -Name 'sched-server-build' -Size '4GB'),
  (New-VolumeFact -Name 'engine-linux-src-notacheckout' -Size '1GB')
)
$holders = ConvertFrom-ContainerMounts -Lines @(
  "engine-linux-0123456789ab-4242-linux-clang-debug|running|$(Get-CheckoutVolumeName -Role src -Root $roots.held),engine-linux-fetch-cache"
)

$verdicts = Get-VolumeVerdict -Volumes $volumes -Checkouts $checkouts -Holders $holders -CurrentRoot $roots.current
$keepUnknown = Get-VolumeVerdict -Volumes $volumes -Checkouts $checkouts -Holders $holders -CurrentRoot $roots.current -KeepUnknown

function Get-V([string]$role, [string]$root) { Find-Verdict $verdicts (Get-CheckoutVolumeName -Role $role -Root $root) }

# --- the stale rule ----------------------------------------------------------------------------------

Test-That 'a worktree gone from disk (git still lists it, prunable): stale' {
  $v = Get-V src $roots.prunable; $v.Stale -and $v.Reason -match 'no checkout there' -and $v.Root -eq $roots.prunable
}
Test-That 'a labelled volume whose root is gone and no longer listed: stale, its root named from the label' {
  $v = Get-V deps $roots.vanished; $v.Stale -and $v.Labelled -and $v.Root -eq $roots.vanished
}
Test-That 'a merged worktree, unlocked and clean: both volumes stale, the branch named' {
  $s = Get-V src $roots.merged; $d = Get-V deps $roots.merged
  $s.Stale -and $d.Stale -and $s.Reason -match 'worktree-agent-merged is merged into main'
}
Test-That 'a merged worktree on a detached HEAD: stale, the commit named' {
  $v = Get-V src $roots.detached; $v.Stale -and $v.Reason -match 'detached HEAD 01234567'
}
Test-That 'a worktree whose commits main holds by patch (a rebase merge): stale, and the reason says so' {
  $v = Get-V src $roots.rebased; $v.Stale -and $v.Reason -match 'worktree-agent-rebased is in main as rebased commits'
}
Test-That 'the main checkout: kept' {
  $s = Get-V src $main; $d = Get-V deps $main
  -not $s.Stale -and -not $d.Stale -and $s.Reason -eq 'the main checkout' -and $s.Root -eq $main
}
Test-That 'an unmerged worktree: kept' {
  $v = Get-V src $roots.unmerged; -not $v.Stale -and $v.Reason -match 'has commits main does not'
}
Test-That 'a merged worktree that is locked (a live agent''s): kept, the lock''s reason shown' {
  $v = Get-V src $roots.locked; -not $v.Stale -and $v.Reason -match 'locked \(claude agent'
}
Test-That 'a merged worktree with uncommitted changes: kept' {
  $v = Get-V src $roots.dirty; -not $v.Stale -and $v.Reason -match 'uncommitted'
}
Test-That 'a volume a running container holds: kept, the container named, whatever its checkout' {
  $v = Get-V src $roots.held
  -not $v.Stale -and $v.Reason -match 'in use by engine-linux-0123456789ab-4242-linux-clang-debug \(running\)'
}
Test-That 'the checkout running the sweep: kept, even merged' {
  $v = Get-V src $roots.current; -not $v.Stale -and $v.Reason -match 'running this sweep'
}
Test-That 'a live checkout this repository does not list (another clone): kept' {
  $v = Get-V src $roots.foreign; -not $v.Stale -and $v.Reason -match 'does not list'
}
Test-That 'an unlabelled volume is found by its id even when git spelt the root with forward slashes' {
  $v = Get-V src $roots.slashes; -not $v.Stale -and $v.Root -and $v.Reason -match 'has commits main does not'
}
Test-That 'an unlabelled volume whose id is no checkout''s: an unknown checkout, stale by default' {
  $s = Find-Verdict $verdicts "engine-linux-src-$orphanId"; $d = Find-Verdict $verdicts "engine-linux-deps-$orphanId"
  $s.Stale -and $d.Stale -and $null -eq $s.Root -and $s.Reason -match 'unknown checkout'
}
Test-That 'and kept with -KeepUnknown, which changes nothing else' {
  $s = Find-Verdict $keepUnknown "engine-linux-src-$orphanId"
  $others = @($keepUnknown | Where-Object { $_.Name -notmatch $orphanId })
  $before = @($verdicts | Where-Object { $_.Name -notmatch $orphanId })
  -not $s.Stale -and $s.Reason -match 'KeepUnknown' -and
    (($others | ForEach-Object { "$($_.Name)=$($_.Stale)" }) -join ';') -eq (($before | ForEach-Object { "$($_.Name)=$($_.Stale)" }) -join ';')
}
Test-That 'the fetch cache, buildx, other projects and look-alike names are never listed' {
  $names = @($verdicts | ForEach-Object { $_.Name })
  -not ($names -contains 'engine-linux-fetch-cache') -and -not ($names -contains 'buildx_buildkit_desktop-linux_state') -and
    -not ($names -contains 'sched-server-build') -and -not ($names -contains 'engine-linux-src-notacheckout')
}
Test-That 'every checkout volume gets exactly one verdict' {
  $verdicts.Count -eq 17 -and @($verdicts | Select-Object -ExpandProperty Name -Unique).Count -eq 17
}
Test-That 'exactly the stale set is stale' {
  $stale = @($verdicts | Where-Object Stale | ForEach-Object { $_.Name } | Sort-Object)
  $want = @((Get-CheckoutVolumeName src $roots.merged), (Get-CheckoutVolumeName deps $roots.merged),
            (Get-CheckoutVolumeName src $roots.detached), (Get-CheckoutVolumeName src $roots.prunable), (Get-CheckoutVolumeName src $roots.rebased),
            (Get-CheckoutVolumeName deps $roots.vanished), "engine-linux-src-$orphanId", "engine-linux-deps-$orphanId") | Sort-Object
  ($stale -join ',') -eq ($want -join ',')
}
Test-That 'a volume whose checkout was never examined is kept, not guessed about' {
  $odd = New-Vol src 'Q:\never\examined' -Labelled
  $v = (Get-VolumeVerdict -Volumes @($odd) -Checkouts $checkouts)[0]
  -not $v.Stale -and $v.Reason -match 'not examined'
}
Test-That 'no volumes, no verdicts' { $none = Get-VolumeVerdict -Volumes @() -Checkouts $checkouts; $none.Count -eq 0 -and $none -is [array] }
Test-That 'the base is named in the reason' {
  $v = (Get-VolumeVerdict -Volumes @((New-Vol src $roots.merged -Labelled)) -Checkouts $checkouts -Base 'origin/main')[0]
  $v.Stale -and $v.Reason -match 'merged into origin/main'
}

# --- -Prune without -Stale -----------------------------------------------------------------------------

Test-That '-Prune names exactly this checkout''s two volumes, labelled or not' {
  $own = Get-OwnVolumeVerdict -Volumes $volumes -Root $roots.merged
  $names = @($own | ForEach-Object { $_.Name } | Sort-Object)
  $own.Count -eq 2 -and ($own | Where-Object { -not $_.Stale }).Count -eq 0 -and
    ($names -join ',') -eq ((@((Get-CheckoutVolumeName deps $roots.merged), (Get-CheckoutVolumeName src $roots.merged)) | Sort-Object) -join ',')
}
Test-That 'and keeps one a container holds' {
  $own = Get-OwnVolumeVerdict -Volumes $volumes -Root $roots.held -Holders $holders
  $own.Count -eq 1 -and -not $own[0].Stale -and $own[0].Reason -match 'in use by'
}

# --- the pieces it rests on -------------------------------------------------------------------------

Test-That 'the volume id is the one the script has always used (D:\workspace\game_engine -> 049fb17c077a)' {
  # Measured on 2026-10-07: the main checkout's volumes on this machine are engine-linux-*-049fb17c077a.
  # If this changes, every unlabelled volume on every machine becomes an unknown checkout.
  (Get-CheckoutVolumeId 'D:\workspace\game_engine') -eq '049fb17c077a' -and
    (Get-CheckoutVolumeId 'd:\WORKSPACE\game_engine') -eq '049fb17c077a'
}
Test-That 'a checkout volume''s labels name its root, its role, its scope and a UTC time' {
  $l = Get-VolumeLabels -Role deps -Root $main -Created ([DateTime]::new(2026, 10, 7, 12, 30, 5, [DateTimeKind]::Utc))
  $l['engine.root'] -eq $main -and $l['engine.role'] -eq 'deps' -and $l['engine.scope'] -eq 'checkout' -and
    $l['engine.created'] -eq '2026-10-07T12:30:05Z'
}
Test-That 'the fetch cache is labelled shared and names no checkout' {
  $l = Get-VolumeLabels -Role fetch-cache
  $l['engine.scope'] -eq 'shared' -and $l['engine.role'] -eq 'fetch-cache' -and -not $l.Contains('engine.root')
}
Test-That 'the porcelain parser: main first, branches short, detached, locked with its reason, prunable' {
  $w = ConvertFrom-WorktreePorcelain -Lines @(
    'worktree D:/work/engine', 'HEAD aaaa', 'branch refs/heads/main', '',
    'worktree D:/work/engine/.claude/worktrees/a', 'HEAD bbbb', 'branch refs/heads/worktree-a',
    'locked claude agent a (pid 7)', '',
    'worktree D:/work/engine/.claude/worktrees/b', 'HEAD cccc', 'detached', 'prunable gitdir file points to non-existent location', '')
  $w.Count -eq 3 -and $w[0].IsMain -and $w[0].Branch -eq 'main' -and -not $w[1].IsMain -and
    $w[1].Branch -eq 'worktree-a' -and $w[1].Locked -and $w[1].LockReason -eq 'claude agent a (pid 7)' -and
    $null -eq $w[2].Branch -and $w[2].Head -eq 'cccc' -and $w[2].Prunable -and $w[2].Path -eq 'D:/work/engine/.claude/worktrees/b'
}
Test-That 'the parser takes CRLF and a missing final blank line' {
  $w = ConvertFrom-WorktreePorcelain -Lines @("worktree /src/engine`r", "HEAD dddd`r", "branch refs/heads/main`r")
  $w.Count -eq 1 -and $w[0].Path -eq '/src/engine' -and $w[0].Branch -eq 'main'
}
Test-That 'container mounts: each volume maps to every container holding it, with its state' {
  $h = ConvertFrom-ContainerMounts -Lines @('a|running|v1,engine-linux-fetch-cache', 'b|exited|v1', 'c|running|', 'garbage')
  $h['v1'] -eq 'a (running), b (exited)' -and $h['engine-linux-fetch-cache'] -eq 'a (running)' -and $h.Count -eq 2
}
Test-That 'docker''s decimal sizes parse, and an unknown one is $null' {
  (ConvertFrom-DockerSize '1.608GB') -eq 1.608e9 -and (ConvertFrom-DockerSize '0B') -eq 0 -and
    (ConvertFrom-DockerSize '35.4kB') -eq 35400 -and (ConvertFrom-DockerSize '218MB') -eq 218e6 -and
    $null -eq (ConvertFrom-DockerSize 'N/A') -and $null -eq (ConvertFrom-DockerSize '')
}
Test-That 'sizes print in docker''s units' {
  (Format-DockerSize 568e9) -eq '568GB' -and (Format-DockerSize 1.608e9) -eq '1.61GB' -and (Format-DockerSize 0) -eq '0B'
}
Test-That 'a verdict prints as one line: action, name, size, checkout or "unknown checkout", reason' {
  $line = Format-VolumeVerdict (Find-Verdict $verdicts "engine-linux-src-$orphanId")
  $line -match "^stale\s+engine-linux-src-$orphanId\s+3\.1GB\s+unknown checkout\s+-- unknown checkout" -and $line -notmatch "`n"
}
Test-That 'a git-spelt root becomes the spelling $Root has' {
  $r = ConvertTo-CheckoutRoot (Join-Path $PSScriptRoot '..')
  $r -eq (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

# --- the script's own refusals, before docker is asked anything ---------------------------------------

function Invoke-Script([string[]]$arguments, [hashtable]$environment = @{}) {
  $saved = @{}
  foreach ($k in $environment.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $environment[$k]) }
  try { $out = & pwsh -NoProfile -File $buildScript @arguments 2>&1; $code = $LASTEXITCODE }
  finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
  $lines = @($out | ForEach-Object { "$_" } | Where-Object { $_.Trim() })
  return [pscustomobject]@{ Code = $code; Lines = $lines }
}
function Test-OneLine($r) {
  # One line, no PowerShell error record: no "At ...:line", no "Line |", no CategoryInfo, no stack.
  $r.Code -ne 0 -and $r.Lines.Count -eq 1 -and $r.Lines[0] -match '^linux-build: ' -and
    -not ($r.Lines -match 'ScriptStackTrace|CategoryInfo|Line \||At .*:\d+ char:')
}

Test-That '-Stale without -Prune: one line, exit 1' { Test-OneLine (Invoke-Script @('-Stale')) }
Test-That '-WhatIf on a build: one line, exit 1' { Test-OneLine (Invoke-Script @('-Preset', 'linux-clang-debug', '-WhatIf')) }
Test-That 'docker unreachable: one line, exit 1, no stack trace' {
  # A port nobody listens on, so the CLI fails at once; or, where docker is not installed (the
  # Linux CI container), the not-on-PATH line. Either is one line.
  $r = Invoke-Script @('-Prune', '-Stale', '-WhatIf') @{ DOCKER_HOST = 'tcp://127.0.0.1:9'; DOCKER_CONTEXT = $null }
  (Test-OneLine $r) -and $r.Lines[0] -match 'docker'
}

# --- report ---------------------------------------------------------------------------------------------

if (Get-PSCallStack | Where-Object { $_.Command -eq 'Invoke-Pester' }) {
  Describe 'tools/linux-build.ps1 -Prune and its volume sweep' {
    foreach ($r in $results) {
      It $r.What -TestCases @(@{ Ok = $r.Ok; Detail = $r.Detail }) {
        param($Ok, $Detail)
        if (-not $Ok) { throw "check failed$(if ($Detail) { ": $Detail" })" }
      }
    }
  }
  return
}

$failed = @($results | Where-Object { -not $_.Ok })
foreach ($r in $results) {
  if ($r.Ok) { Write-Host "  ok   $($r.What)" }
  else { Write-Host "  FAIL $($r.What)$(if ($r.Detail) { " (threw: $($r.Detail))" })" -ForegroundColor Red }
}
Write-Host ''
if ($failed.Count -gt 0) {
  Write-Host "$($failed.Count) of $($results.Count) checks FAILED" -ForegroundColor Red
  exit 1
}
Write-Host "all $($results.Count) checks passed"
exit 0
