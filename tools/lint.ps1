#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Banned-pattern lint for engine sources (docs/plan/11-performance-principles.md section 11.2).

.DESCRIPTION
  Scans engine source directories for node-based standard containers and ownership
  types that are replaced by core/containers, exception usage, and includes that are
  confined to part of the tree, and fails if any appear outside the allowed locations.
  A line may opt out with a trailing comment:

      // engine-lint: allow-std-container <reason>
      // engine-lint: allow-exceptions <reason>
      // engine-lint: allow-flecs <reason>

  A file may opt out entirely with the same marker anywhere in its first 5 lines.

  Allowed locations without a marker: tools/, tests/ directories, bench/ directories,
  third_party/, build/ — except for rules that name their own `AllowedPaths`, which
  are confined to those paths wherever they appear.
#>
[CmdletBinding()]
param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$engineDirs = @('core', 'foundation', 'domain', 'systems', 'apps', 'game') |
  ForEach-Object { Join-Path $Root $_ } | Where-Object { Test-Path $_ }

# A rule with `AllowedPaths` is a *confinement* rule: the pattern is legal inside those
# directories — including their tests and benches — and illegal everywhere else in the engine
# tree. `Scope = 'all'` says so, because the tests/ and bench/ exemption that makes sense for a
# container choice makes no sense for an include that decides which modules a library reaches.
$rules = @(
  @{ Name = 'std-container'; Marker = 'allow-std-container'; Scope = 'engine'
     Pattern = 'std::(map|multimap|set|multiset|unordered_map|unordered_multimap|unordered_set|unordered_multiset|list|forward_list|deque|shared_ptr|weak_ptr|make_shared)\b'
     Message = 'banned node-based container or shared ownership; use core/containers or unique ownership' },
  @{ Name = 'exceptions'; Marker = 'allow-exceptions'; Scope = 'engine'
     Pattern = '^\s*(throw\b|try\s*\{|catch\s*\()|\bstd::(exception|runtime_error|logic_error)\b'
     Message = 'exceptions are not used across engine code; return a result' },
  # ADR-0028 seam 5. flecs is exposed directly and deliberately not wrapped, which only stays a
  # bounded decision if the set of modules that can see it is bounded: the cost of ever replacing
  # it is "rewrite the system bodies and queries", and that is only true while physics, nav, anim,
  # gfx, geometry, sim, store and everything in core/ and foundation/ have never heard of it.
  # Bridging code between an ECS-free module and the world lives in systems/.
  @{ Name = 'flecs-include'; Marker = 'allow-flecs'; Scope = 'all'
     Pattern = '^\s*#\s*include\s*[<"]flecs'
     AllowedPaths = @('domain/ecs', 'systems', 'game')
     Message = 'flecs.h belongs to domain/ecs, systems/ and game/ (ADR-0028 seam 5); lower modules stay ECS-free and bridging code lives in systems/' }
)

$violations = New-Object System.Collections.Generic.List[string]
$scanned = 0

# A path is inside one of a rule's allowed roots. Compared on the repository-relative path with
# forward slashes, so the answer does not depend on the platform's separator.
function Test-AllowedPath([string]$relative, [string[]]$allowed) {
  foreach ($prefix in $allowed) {
    if ($relative -eq $prefix -or $relative.StartsWith("$prefix/")) { return $true }
  }
  return $false
}

foreach ($dir in $engineDirs) {
  $files = Get-ChildItem -Path $dir -Recurse -Include *.h, *.hpp, *.cpp, *.inl -File |
    Where-Object { $_.FullName -notmatch '[\\/](third_party|build|_deps)[\\/]' }
  foreach ($file in $files) {
    # Reported with forward slashes whatever the platform, so a message is the same string on
    # Windows and on CI's Linux runners and a test can assert on it.
    $rel = ([IO.Path]::GetRelativePath($Root, $file.FullName)) -replace '\\', '/'
    $isTestOrBench = $file.FullName -match '[\\/](tests|bench)[\\/]'
    $scanned++
    $lines = Get-Content -LiteralPath $file.FullName
    $head = ($lines | Select-Object -First 5) -join "`n"
    foreach ($rule in $rules) {
      if ($rule.Scope -ne 'all' -and $isTestOrBench) { continue }
      if ($rule.AllowedPaths -and (Test-AllowedPath $rel $rule.AllowedPaths)) { continue }
      if ($head -match "engine-lint:\s*$($rule.Marker)") { continue }
      for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match $rule.Pattern -and $line -notmatch "engine-lint:\s*$($rule.Marker)") {
          $violations.Add(("{0}:{1}: [{2}] {3}`n    {4}" -f $rel, ($i + 1), $rule.Name, $rule.Message, $line.Trim()))
        }
      }
    }
  }
}

if ($violations.Count -gt 0) {
  Write-Host "lint: $($violations.Count) violation(s) in $scanned file(s)" -ForegroundColor Red
  $violations | ForEach-Object { Write-Host $_ }
  exit 1
}

Write-Host "lint: OK ($scanned engine source files scanned)"
exit 0
