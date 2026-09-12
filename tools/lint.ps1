#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Banned-pattern lint for engine sources (docs/plan/11-performance-principles.md section 11.2).

.DESCRIPTION
  Scans engine source directories for node-based standard containers and ownership
  types that are replaced by core/containers, plus exception usage, and fails if any
  appear outside the allowed locations. A line may opt out with a trailing comment:

      // engine-lint: allow-std-container <reason>
      // engine-lint: allow-exceptions <reason>

  A file may opt out entirely with the same marker anywhere in its first 5 lines.

  Allowed locations without a marker: tools/, tests/ directories, bench/ directories,
  third_party/, build/.
#>
[CmdletBinding()]
param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$engineDirs = @('core', 'foundation', 'domain', 'systems', 'apps', 'game') |
  ForEach-Object { Join-Path $Root $_ } | Where-Object { Test-Path $_ }

$rules = @(
  @{ Name = 'std-container'; Marker = 'allow-std-container'
     Pattern = 'std::(map|multimap|set|multiset|unordered_map|unordered_multimap|unordered_set|unordered_multiset|list|forward_list|deque|shared_ptr|weak_ptr|make_shared)\b'
     Message = 'banned node-based container or shared ownership; use core/containers or unique ownership' },
  @{ Name = 'exceptions'; Marker = 'allow-exceptions'
     Pattern = '^\s*(throw\b|try\s*\{|catch\s*\()|\bstd::(exception|runtime_error|logic_error)\b'
     Message = 'exceptions are not used across engine code; return a result' }
)

$violations = New-Object System.Collections.Generic.List[string]
$scanned = 0

foreach ($dir in $engineDirs) {
  $files = Get-ChildItem -Path $dir -Recurse -Include *.h, *.hpp, *.cpp, *.inl -File |
    Where-Object { $_.FullName -notmatch '[\\/](tests|bench|third_party|build|_deps)[\\/]' }
  foreach ($file in $files) {
    $scanned++
    $lines = Get-Content -LiteralPath $file.FullName
    $head = ($lines | Select-Object -First 5) -join "`n"
    foreach ($rule in $rules) {
      if ($head -match "engine-lint:\s*$($rule.Marker)") { continue }
      for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match $rule.Pattern -and $line -notmatch "engine-lint:\s*$($rule.Marker)") {
          $rel = [IO.Path]::GetRelativePath($Root, $file.FullName)
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
