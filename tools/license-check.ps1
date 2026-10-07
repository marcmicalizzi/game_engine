#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The licence check ADR-0014 promised: refuses a dependency whose licence is not on the permissive
  list, or that nobody recorded.

.DESCRIPTION
  tools/license-check.ps1 [-Root <repo>] [-BuildDir <build/<preset>>]

  Reads third_party/LICENSES.md (the record), every FetchContent_Declare under cmake/ and in the
  CMakeLists.txt files (what a configure downloads), and every directory vendored under third_party/,
  and fails naming the dependency when:

  - a row's licence is not one ADR-0014 allows (MIT, BSD, zlib, Apache-2.0, BSL-1.0, public domain;
    LGPL only where the row says the library is dynamically linked; never GPL, AGPL, SSPL, BUSL or
    anything proprietary or source-available);
  - a declaration or a vendored directory has no row, or a row names a declaration that no longer
    exists;
  - with -BuildDir (a configured build tree, whose _deps/<name>-src hold the fetched sources), a
    dependency's own licence file is under a licence its row does not name.

  The rules are tools/lib/LicenseCheck.psm1's, tested by tools/license-check.Tests.ps1. It runs in
  `tools/dev.ps1 lint`, as the CTest test `lint.licenses` in every preset (with that preset's build
  tree, so CI reads the fetched licence files too), and in CI's documentation job, which has no
  build and checks the record and the declarations alone.

  Exit codes: 0 clean; 1 a problem, each printed with the file and line to fix.
#>
[CmdletBinding()]
param(
  [string]$Root = '',
  [string]$BuildDir = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
Import-Module (Join-Path $PSScriptRoot 'lib/LicenseCheck.psm1') -Force

$sources = @{}
if ($BuildDir) {
  $deps = Join-Path $BuildDir '_deps'
  foreach ($d in (Find-FetchContentDeclarations -Root $Root)) {
    $src = Join-Path $deps ("$($d.Name)-src".ToLowerInvariant())
    if (Test-Path -LiteralPath $src) { $sources[$d.Name] = $src }
  }
}
$result = Invoke-LicenseCheck -Root $Root -SourceDirs $sources
$rows = @($result.Rows).Count
$decls = @($result.Declarations).Count
foreach ($n in $result.Notes) { Write-Host "license-check: note: $n" }
if ($result.Problems.Count -gt 0) {
  foreach ($p in $result.Problems) { Write-Host "license-check: $p" -ForegroundColor Red }
  Write-Host "license-check: $($result.Problems.Count) problem(s) over $rows recorded dependencies and $decls declarations" -ForegroundColor Red
  exit 1
}
$read = $sources.Count
Write-Host "license-check: $rows recorded dependencies, $decls declarations, $read dependencies' licence files read: every licence on ADR-0014's list"
exit 0
