#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for the licence check (tools/lib/LicenseCheck.psm1, tools/license-check.ps1). Runs under
  CTest as `tools.license_check`.

.DESCRIPTION
  The check is what keeps ADR-0014's promise, so its rules are held over fixture trees written
  here: the policy over licence expressions (alternatives, conjunctions, LGPL's one exception),
  a declaration nobody recorded, a record of nothing, vendored code, and a licence file that
  disagrees with its row — and that a licence text which only mentions the GPL in passing (the
  Apache-2.0 LLVM exception does) is not read as the GPL. Then the script runs over this tree.

      pwsh tools/license-check.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'lib/LicenseCheck.psm1') -Force

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { Write-Host "       threw: $($_.Exception.Message)"; $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}
function Ok([string]$license, [string]$how = '') { (Test-LicenseExpression -License $license -HowObtained $how).Ok }

Write-Host 'the policy over a row''s licence'
Test-That 'each licence ADR-0014 names is allowed' {
  (Ok 'MIT') -and (Ok 'BSD-3-Clause') -and (Ok 'zlib') -and (Ok 'Apache-2.0') -and (Ok 'BSL-1.0') -and (Ok 'public domain') -and
  (Ok 'Unlicense') -and (Ok 'Apache-2.0 WITH LLVM-exception') -and (Ok 'Unlicense (public domain) OR MIT-0')
}
Test-That 'GPL, AGPL, SSPL, BUSL and proprietary are refused' {
  -not (Ok 'GPL-3.0') -and -not (Ok 'GPL-2.0-or-later') -and -not (Ok 'AGPL-3.0') -and -not (Ok 'SSPL-1.0') -and
  -not (Ok 'BUSL-1.1') -and -not (Ok 'proprietary') -and -not (Ok 'source-available')
}
Test-That 'one allowed alternative is enough' { (Ok 'MIT OR GPL-3.0') -and (Ok 'GPL-2.0 OR Apache-2.0') }
Test-That 'licences that all apply need every one allowed' { (Ok 'MIT plus BSD-3-Clause') -and -not (Ok 'MIT plus GPL-2.0') -and -not (Ok 'MIT AND LGPL-2.1') }
Test-That 'LGPL only where the row says it is dynamically linked' {
  -not (Ok 'LGPL-2.1') -and (Ok 'LGPL-2.1' 'a shared library, dynamically linked and user-replaceable')
}
Test-That 'an expression naming no licence is refused rather than passed' { -not (Ok 'see the website') }
Test-That 'AGPL and LGPL are not read as GPL' {
  ((Get-LicenseFamilies 'AGPL-3.0') -join ',') -eq 'AGPL' -and ((Get-LicenseFamilies 'LGPL-2.1') -join ',') -eq 'LGPL'
}

Write-Host 'a licence file''s text'
$llvm = "Apache License, Version 2.0`n---- LLVM Exceptions to the Apache 2.0 License ----`nIn addition, if you combine or link compiled forms of this Software with software that is licensed under the GPLv2 (""Combined Software"") ..."
Test-That 'the LLVM exception''s mention of the GPLv2 is not the GPL' { ((Get-LicenseFamilies -File $llvm) -join ',') -eq 'Apache-2.0' }
Test-That 'the GPL''s own title is' { (Get-LicenseFamilies -File "GNU GENERAL PUBLIC LICENSE`nVersion 3, 29 June 2007") -contains 'GPL' }
Test-That 'MIT, zlib, BSD and the Unlicense by their wording' {
  ((Get-LicenseFamilies -File 'Permission is hereby granted, free of charge, to any person') -join ',') -eq 'MIT' -and
  ((Get-LicenseFamilies -File '2. Altered source versions must be plainly marked as such') -join ',') -eq 'zlib' -and
  ((Get-LicenseFamilies -File 'Redistribution and use in source and binary forms, with or without') -join ',') -eq 'BSD' -and
  ((Get-LicenseFamilies -File 'This is free and unencumbered software released into the public domain.') -join ',') -eq 'public-domain'
}

Write-Host 'a tree'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("engine-license-check-" + [guid]::NewGuid().ToString('N'))
function New-Tree([string[]]$rows, [string[]]$declares, [hashtable]$licenseFiles = @{}) {
  $t = Join-Path $scratch ([guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path (Join-Path $t 'third_party'), (Join-Path $t 'cmake') | Out-Null
  $md = @('# Third-party components', '', '| Component | Version | License | Used by | How obtained | Declared as |', '|---|---|---|---|---|---|') + $rows +
        @('', '## Consulted, not vendored', '', '| Work | License | What was taken | Where it is used |', '|---|---|---|---|', '| A paper | GPL-3.0 | an idea | nowhere |')
  $md -join "`n" | Set-Content (Join-Path $t 'third_party/LICENSES.md')
  ($declares | ForEach-Object { "FetchContent_Declare($_`n  GIT_REPOSITORY https://example.invalid/$_.git)" }) -join "`n" | Set-Content (Join-Path $t 'cmake/Deps.cmake')
  '# FetchContent_Declare(commented_out' | Set-Content (Join-Path $t 'CMakeLists.txt')
  $dirs = @{}
  foreach ($name in $licenseFiles.Keys) {
    $src = Join-Path $t "_deps/$name-src"
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    $licenseFiles[$name] | Set-Content (Join-Path $src 'LICENSE')
    $dirs[$name] = $src
  }
  return @{ Root = $t; Sources = $dirs }
}
function Row([string]$name, [string]$license, [string]$declared = $name) { "| $name | 1.0 | $license | domain/x | FetchContent | ``$declared`` |" }
$mit = 'MIT License. Permission is hereby granted, free of charge, to any person obtaining a copy'
try {
  $t = New-Tree @((Row 'alpha' 'MIT'), (Row 'beta' 'zlib')) @('alpha', 'beta') @{ alpha = $mit; beta = 'Altered source versions must be plainly marked as such.' }
  $r = Invoke-LicenseCheck -Root $t.Root -SourceDirs $t.Sources
  Test-That 'a recorded, allowed, agreeing tree is clean, and a commented-out declaration is not one' { $r.Problems.Count -eq 0 -and @($r.Declarations).Count -eq 2 }
  Test-That 'the consulted works'' table is not the record' { @($r.Rows).Count -eq 2 }

  $t = New-Tree @((Row 'alpha' 'MIT')) @('alpha', 'gamma')
  $r = Invoke-LicenseCheck -Root $t.Root
  Test-That 'a declaration nobody recorded fails, naming it and where it is declared' {
    $r.Problems.Count -eq 1 -and $r.Problems[0] -match '^cmake/Deps\.cmake:\d+: FetchContent_Declare\(gamma\)'
  }
  Test-That 'licence files it could not read are a note, not a pass in silence' { @($r.Notes | Where-Object { $_ -match 'not read.*alpha' }).Count -eq 1 }

  $t = New-Tree @((Row 'alpha' 'MIT'), (Row 'old' 'MIT')) @('alpha')
  $r = Invoke-LicenseCheck -Root $t.Root
  Test-That 'a row naming a declaration that no longer exists fails' { $r.Problems.Count -eq 1 -and $r.Problems[0] -match 'old is recorded as `old`, which nothing declares' }

  $t = New-Tree @((Row 'alpha' 'GPL-3.0')) @('alpha')
  $r = Invoke-LicenseCheck -Root $t.Root
  Test-That 'a recorded licence the policy refuses fails, naming the dependency' { $r.Problems.Count -eq 1 -and $r.Problems[0] -match 'alpha is under GPL, which ADR-0014 refuses' }

  $t = New-Tree @((Row 'alpha' 'MIT')) @('alpha') @{ alpha = "GNU GENERAL PUBLIC LICENSE`nVersion 3" }
  $r = Invoke-LicenseCheck -Root $t.Root -SourceDirs $t.Sources
  Test-That 'code under the GPL recorded as MIT fails on its licence file' { $r.Problems.Count -eq 1 -and $r.Problems[0] -match '^alpha: LICENSE is under GPL' }

  $t = New-Tree @((Row 'alpha' 'MIT')) @('alpha') @{ alpha = 'Redistribution and use in source and binary forms' }
  $r = Invoke-LicenseCheck -Root $t.Root -SourceDirs $t.Sources
  Test-That 'an allowed licence the row does not name is a wrong row' { $r.Problems.Count -eq 1 -and $r.Problems[0] -match "alpha: LICENSE reads as BSD.*says 'MIT'" }

  $t = New-Tree @((Row 'alpha' 'MIT')) @('alpha')
  New-Item -ItemType Directory -Force -Path (Join-Path $t.Root 'third_party/vendored_thing') | Out-Null
  $r = Invoke-LicenseCheck -Root $t.Root
  Test-That 'a directory vendored under third_party/ needs a row too' { @($r.Problems | Where-Object { $_ -match 'third_party/vendored_thing: vendored code' }).Count -eq 1 }

  $t = New-Tree @("| alpha | 1.0 | MIT | domain/x | FetchContent |") @('alpha')
  Test-That 'a record without the declared-as column is refused with a sentence' {
    try { Invoke-LicenseCheck -Root $t.Root | Out-Null; $false } catch { $_.Exception.Message -match 'declared or vendored as' }
  }
} finally {
  Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue
}

Write-Host 'this tree'
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'license-check.ps1') -Root $Root *> $null
Test-That 'its record, declarations and vendored code pass the check' { $LASTEXITCODE -eq 0 }

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0
