<#
  The licence check (ADR-0014; docs/plan/08-toolchain.md §8.8): the rules tools/license-check.ps1
  applies, kept apart from the tree it reads so tools/license-check.Tests.ps1 can hold them over
  fixtures.

  Three sources say what code the engine takes in, and the check holds them to each other and to
  the policy:

  - third_party/LICENSES.md, the record: one row per dependency, its licence as a short expression
    ("MIT", "Apache-2.0 OR MIT", "MIT OR Unlicense (public domain)") and, in its last column, the
    name its `FetchContent_Declare` gives it (or the directory it is vendored in under third_party/).
  - Every `FetchContent_Declare(<name> ...)` under cmake/ and in any CMakeLists.txt: what a configure
    actually downloads. A declaration with no row is a dependency nobody recorded; a row naming a
    declaration that no longer exists is a record of nothing.
  - The dependency's own licence files, where its sources are on disk (a configured build tree's
    _deps/<name>-src, or a directory vendored under third_party/): the licence the code says it is
    under. A file whose licence the row does not name is a row that is wrong.

  The policy, ADR-0014's: MIT (and MIT-0), BSD, zlib, Apache-2.0, BSL-1.0 and public domain
  (Unlicense, CC0) are allowed; LGPL only as a dynamically linked, user-replaceable library, which
  a row has to say it is; GPL, AGPL, SSPL, the Business Source License and anything proprietary or
  source-available are refused. A row offering alternatives ("A OR B") is allowed when one of them
  is; a row whose licences all apply ("A plus B", "A AND B") needs every one allowed.
#>

Set-StrictMode -Version Latest

$script:Allowed = @('MIT', 'BSD', 'Apache-2.0', 'zlib', 'BSL-1.0', 'public-domain')
$script:Refused = @('GPL', 'AGPL', 'LGPL', 'SSPL', 'BUSL', 'proprietary')

# The licence families an expression names, in the record's vocabulary (SPDX identifiers and the
# words the record uses for them). Order matters only in that AGPL and LGPL are found before GPL.
#
# -File reads a licence file's text instead, by the licences' own wording and titles: a licence
# text mentions others in passing (the Apache-2.0 LLVM exception names the GPLv2 it is compatible
# with), so a file is under GPL when it carries the GPL's title, not when it says "GPL".
function Get-LicenseFamilies {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [AllowEmptyString()] [string]$Text, [switch]$File)
  $found = New-Object System.Collections.Generic.List[string]
  $t = $Text
  if ($File) {
    $rules = @(
      @{ Family = 'AGPL'; Pattern = '(?-i)GNU AFFERO GENERAL PUBLIC LICENSE' },
      @{ Family = 'LGPL'; Pattern = '(?-i)GNU (LESSER|LIBRARY) GENERAL PUBLIC LICENSE' },
      @{ Family = 'GPL'; Pattern = '(?-i)GNU GENERAL PUBLIC LICENSE' },
      @{ Family = 'SSPL'; Pattern = '(?-i)Server Side Public License' },
      @{ Family = 'BUSL'; Pattern = '(?-i)Business Source License' },
      @{ Family = 'Apache-2.0'; Pattern = 'Apache License,?\s+Version 2\.0|Apache[- ]2\.0' },
      @{ Family = 'BSL-1.0'; Pattern = 'Boost Software License' },
      @{ Family = 'BSD'; Pattern = 'Redistribution and use in source and binary forms' },
      @{ Family = 'zlib'; Pattern = 'altered source versions must be plainly marked' },
      @{ Family = 'MIT'; Pattern = 'Permission is hereby granted, free of charge|\bMIT License\b|\bMIT No Attribution\b' },
      @{ Family = 'public-domain'; Pattern = 'free and unencumbered software released into the public domain|\bpublic domain\b' }
    )
    foreach ($r in $rules) {
      if ($t -match $r.Pattern -and -not $found.Contains($r.Family)) { $found.Add($r.Family) }
    }
    return $found.ToArray()
  }
  $rules = @(
    @{ Family = 'AGPL'; Pattern = '\bAGPL|Affero' },
    @{ Family = 'LGPL'; Pattern = '\bLGPL|Lesser General Public|Library General Public' },
    @{ Family = 'GPL'; Pattern = '(?<![AL])\bGPL|(?<!Lesser |Library |Affero )General Public License' },
    @{ Family = 'SSPL'; Pattern = '\bSSPL|Server Side Public' },
    @{ Family = 'BUSL'; Pattern = '\bBUSL|Business Source License' },
    @{ Family = 'proprietary'; Pattern = '(?i)\bproprietary\b|source-available|\bcommons clause\b' },
    @{ Family = 'Apache-2.0'; Pattern = 'Apache[- ]2(\.0)?|Apache License,? Version 2' },
    @{ Family = 'BSL-1.0'; Pattern = '\bBSL-1\.0\b|Boost Software License' },
    @{ Family = 'BSD'; Pattern = '\bBSD\b|\bBSD-\d-Clause|Redistribution and use in source and binary forms' },
    @{ Family = 'zlib'; Pattern = '(?i)\bzlib\b|altered source versions must be plainly marked' },
    @{ Family = 'MIT'; Pattern = '\bMIT\b|MIT-0|Permission is hereby granted, free of charge' },
    @{ Family = 'public-domain'; Pattern = '(?i)\bpublic domain\b|\bUnlicense\b|\bCC0\b|free and unencumbered software' }
  )
  foreach ($r in $rules) {
    if ($t -match $r.Pattern -and -not $found.Contains($r.Family)) { $found.Add($r.Family) }
  }
  return $found.ToArray()
}

<#
  Whether a row's licence expression is one the policy allows. Returns @{ Ok; Why; Families }.
  `$HowObtained` is the row's "How obtained" cell: LGPL is allowed only where it says the library is
  dynamically linked.
#>
function Test-LicenseExpression {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [AllowEmptyString()] [string]$License, [string]$HowObtained = '')
  $families = @(Get-LicenseFamilies $License)
  if ($families.Count -eq 0) { return @{ Ok = $false; Why = "names no licence the policy knows: '$License'"; Families = $families } }
  $refused = @($families | Where-Object { $_ -in $script:Refused })
  $allowed = @($families | Where-Object { $_ -in $script:Allowed })
  $dynamic = $HowObtained -match '(?i)dynamically linked'
  if ($dynamic) { $refused = @($refused | Where-Object { $_ -ne 'LGPL' }) }
  if ($refused.Count -eq 0) { return @{ Ok = $true; Why = ''; Families = $families } }
  # Alternatives: one allowed choice is enough.
  if ($License -cmatch '\bOR\b' -and $allowed.Count -gt 0) { return @{ Ok = $true; Why = ''; Families = $families } }
  $why = "is under $($refused -join ', '), which ADR-0014 refuses"
  if ('LGPL' -in $refused) { $why += ' (LGPL only as a dynamically linked, user-replaceable library, and the row does not say it is one)' }
  return @{ Ok = $false; Why = $why; Families = $families }
}

# The record's first table: one object per row with Component, Version, License, UsedBy, HowObtained
# and DeclaredAs (the FetchContent names or vendored directories in its last column, backticked).
function Read-LicenseRecord {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [string]$Path)
  $rows = New-Object System.Collections.Generic.List[object]
  $inTable = $false
  $n = 0
  foreach ($line in (Get-Content -LiteralPath $Path)) {
    $n++
    if (-not $inTable) {
      if ($line -match '^\|\s*Component\s*\|') { $inTable = $true; $header = $line }
      continue
    }
    if ($line -match '^\|\s*-') { continue }
    if ($line -notmatch '^\|') { break }
    $cells = @($line.Trim().Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
    if ($cells.Count -lt 6) { throw "license-check: $Path line ${n}: a row of $($cells.Count) cells where the table has 6 (the last is the name it is declared or vendored as)" }
    $declared = @([regex]::Matches($cells[5], '`([^`]+)`') | ForEach-Object { $_.Groups[1].Value })
    $rows.Add([pscustomobject]@{
      Component = $cells[0]; Version = $cells[1]; License = $cells[2]; UsedBy = $cells[3]
      HowObtained = $cells[4]; DeclaredAs = $declared; Line = $n
    })
  }
  if (-not $inTable) { throw "license-check: $Path has no table whose first column is Component" }
  return $rows.ToArray()
}

# Every `FetchContent_Declare(<name>` under the given files: name, file and line.
function Find-FetchContentDeclarations {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [string]$Root)
  $files = @()
  $cmakeDir = Join-Path $Root 'cmake'
  if (Test-Path $cmakeDir) { $files += @(Get-ChildItem -LiteralPath $cmakeDir -Recurse -File -Filter '*.cmake') }
  $files += @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter 'CMakeLists.txt' |
              Where-Object { $_.FullName -notmatch '[\\/](build|_deps|\.git|content|third_party)[\\/]' })
  $out = New-Object System.Collections.Generic.List[object]
  foreach ($f in $files) {
    $n = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
      $n++
      if ($line -match '^\s*#') { continue }
      if ($line -match '\bFetchContent_Declare\(\s*([A-Za-z0-9_\-]+)') {
        $rel = [IO.Path]::GetRelativePath($Root, $f.FullName) -replace '\\', '/'
        $out.Add([pscustomobject]@{ Name = $Matches[1]; File = $rel; Line = $n })
      }
    }
  }
  return $out.ToArray()
}

# A dependency's own licence files at the top of its sources: LICENSE*, LICENCE*, COPYING*,
# UNLICENSE*, and <something>_LICENSE* (Luau's lua_LICENSE.txt).
function Get-LicenseFiles([string]$SourceDir) {
  if (-not (Test-Path -LiteralPath $SourceDir)) { return @() }
  return @(Get-ChildItem -LiteralPath $SourceDir -File | Where-Object {
    $_.Name -match '^(?i)(licen[cs]e|copying|unlicense|notice)(\.[a-z]+)?$|^(?i)[a-z0-9]+_licen[cs]e(\.[a-z]+)?$'
  })
}

<#
  The whole check. Returns @{ Problems = [string[]]; Notes = [string[]]; Rows; Declarations }.
  -SourceDirs maps a declared name to where its sources are, when the caller knows (a configured
  build tree's _deps); a name with no entry has its licence files left unread, and a note says so.
#>
function Invoke-LicenseCheck {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [string]$Root, [hashtable]$SourceDirs = @{})
  $problems = New-Object System.Collections.Generic.List[string]
  $notes = New-Object System.Collections.Generic.List[string]
  $record = Join-Path $Root 'third_party/LICENSES.md'
  $rows = @(Read-LicenseRecord -Path $record)
  $decls = @(Find-FetchContentDeclarations -Root $Root)

  # The record against the policy.
  foreach ($row in $rows) {
    $verdict = Test-LicenseExpression -License $row.License -HowObtained $row.HowObtained
    if (-not $verdict.Ok) { $problems.Add("third_party/LICENSES.md:$($row.Line): $($row.Component) $($verdict.Why)") }
  }

  # What a configure downloads against the record, both ways.
  $byName = @{}
  foreach ($row in $rows) { foreach ($d in $row.DeclaredAs) { $byName[$d.ToLowerInvariant()] = $row } }
  $declared = @{}
  foreach ($d in $decls) {
    $declared[$d.Name.ToLowerInvariant()] = $d
    if (-not $byName.ContainsKey($d.Name.ToLowerInvariant())) {
      $problems.Add("$($d.File):$($d.Line): FetchContent_Declare($($d.Name)) brings code in that third_party/LICENSES.md does not record: add its row, with its licence, before it merges (ADR-0014)")
    }
  }
  # Vendored directories under third_party/ are dependencies too.
  $vendored = @(Get-ChildItem -LiteralPath (Join-Path $Root 'third_party') -Directory -ErrorAction SilentlyContinue)
  foreach ($v in $vendored) {
    $declared[$v.Name.ToLowerInvariant()] = [pscustomobject]@{ Name = $v.Name; File = "third_party/$($v.Name)"; Line = 0 }
    if (-not $byName.ContainsKey($v.Name.ToLowerInvariant())) {
      $problems.Add("third_party/$($v.Name): vendored code that third_party/LICENSES.md does not record")
    }
    if (-not $SourceDirs.ContainsKey($v.Name)) { $SourceDirs[$v.Name] = $v.FullName }
  }
  foreach ($row in $rows) {
    foreach ($name in $row.DeclaredAs) {
      if (-not $declared.ContainsKey($name.ToLowerInvariant())) {
        $problems.Add("third_party/LICENSES.md:$($row.Line): $($row.Component) is recorded as ``$name``, which nothing declares or vendors any more: remove the row or correct the name")
      }
    }
  }

  # The dependency's own licence files against its row.
  $unread = New-Object System.Collections.Generic.List[string]
  foreach ($d in $declared.Values) {
    $row = $byName[$d.Name.ToLowerInvariant()]
    if ($null -eq $row) { continue }
    $dir = $SourceDirs[$d.Name]
    if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { $unread.Add($d.Name); continue }
    $files = @(Get-LicenseFiles $dir)
    if ($files.Count -eq 0) {
      $notes.Add("$($d.Name): no licence file at the top of its sources ($dir); the record says $($row.License)")
      continue
    }
    $named = @(Get-LicenseFamilies $row.License)
    foreach ($f in $files) {
      $found = @(Get-LicenseFamilies -File (Get-Content -LiteralPath $f.FullName -Raw))
      if ($found.Count -eq 0) { $notes.Add("$($d.Name): $($f.Name) names no licence the check recognises"); continue }
      $unrecorded = @($found | Where-Object { $_ -notin $named })
      # A refused licence the row does not offer as an alternative it allows: the code is under it.
      $refused = @($found | Where-Object { $_ -in $script:Refused -and $_ -notin $named })
      if ($refused.Count -gt 0) {
        $problems.Add("$($d.Name): $($f.Name) is under $($refused -join ', '), which ADR-0014 refuses (third_party/LICENSES.md:$($row.Line) says '$($row.License)')")
      } elseif ($unrecorded.Count -gt 0) {
        $problems.Add("$($d.Name): $($f.Name) reads as $($found -join ' and '), and third_party/LICENSES.md:$($row.Line) says '$($row.License)': correct the row, or find out why the code is under a licence the record does not name")
      }
    }
  }
  if ($unread.Count -gt 0) {
    $notes.Add("licence files not read, sources not on disk (configure a preset and pass -BuildDir): $(@($unread | Sort-Object) -join ', ')")
  }
  return @{ Problems = $problems.ToArray(); Notes = $notes.ToArray(); Rows = $rows; Declarations = $decls }
}

Export-ModuleMember -Function Get-LicenseFamilies, Test-LicenseExpression, Read-LicenseRecord,
  Find-FetchContentDeclarations, Get-LicenseFiles, Invoke-LicenseCheck
