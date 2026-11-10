#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Ceilings for structure: the size of the files every feature edits, pinned the way the size
  tables pin a hot type's bytes and the frame budgets a pass's milliseconds.

.DESCRIPTION
  tools/structure-check.ps1 [-Root <tree>] [-Ceilings <file>]
  tools/structure-check.ps1 -Rebaseline [-Reason "<why>"] [-By <who>]

  Reads tools/structure-ceilings.json, measures every hub it names in the tree — the lines of one
  function, the lines of a file, the shader modules the build compiles, the fields of a schema
  struct, the `#if` lines that name a capability's macro, the bytes of a page — and every page the
  file does not name against its default, and fails naming each value over its ceiling.

  A ceiling only goes down. A value that has fallen more than `lower_after_percent` under its
  ceiling fails too, so the slack a shrinking hub leaves cannot let the growth back: lower it with
  -Rebaseline, which needs no reason. A rebaseline that raises any ceiling (or names a page that
  has grown past the default) refuses without -Reason, and with one keeps the reason, the date,
  who, the commit and every old and new value in the file's `rebaselines`. Either writes the file
  only; commit it with the change and say why in the commit message.

  How each measure is counted, and why, is on the rule's page (docs/ci/structure-and-citations.md,
  "Ceilings for structure"). Runs under CTest as `structure_check`;
  tools/structure-check.Tests.ps1 is its own test (`tools.structure_check`).

  Exit codes: 0 every value within its ceiling (or the file rebaselined); 1 a value over or too
  far under its ceiling, a hub that is not there, or a refused rebaseline; 2 a ceilings file that
  cannot be read.

.EXAMPLE
  pwsh tools/structure-check.ps1
  pwsh tools/structure-check.ps1 -Rebaseline
  pwsh tools/structure-check.ps1 -Rebaseline -Reason "batch 58: R69, S100 and R129 grow the renderer's hubs"
#>
[CmdletBinding()]
param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
  [string]$Ceilings = '',
  [switch]$Rebaseline,
  [string]$Reason = '',
  [string]$By = '',
  [string]$Date = (Get-Date -Format 'yyyy-MM-dd')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path -LiteralPath $Root).Path
if (-not $Ceilings) { $Ceilings = Join-Path $Root 'tools/structure-ceilings.json' }

# The same directories docs-check skips: build output, vendored code, worktrees and caches, and
# the downloaded samples, whose pages are somebody else's.
$excludedDirs = @('build', 'out', '_deps', 'third_party', '.git', '.claude', '.cache', 'node_modules', 'ddc')
$excludedPaths = @('content/samples')
$layerDirs = @('core', 'foundation', 'domain', 'systems', 'apps', 'game')
$sourceExtensions = @('.c', '.cc', '.cpp', '.h', '.hpp', '.inc')

$units = @{
  'function-lines' = 'lines'; 'function-matches' = 'matches'; 'file-lines' = 'lines'
  'shader-modules' = 'modules'; 'schema-fields' = 'fields'; 'conditionals' = '#if lines'; 'page-bytes' = 'bytes'
}

function Get-Rel([string]$path) { return ([IO.Path]::GetRelativePath($Root, $path) -replace '\\', '/') }

# A tree path as the ceilings file writes it, `/`-separated and in the tree's own case, resolved
# one component at a time with an ordinal comparison: Windows would open `Main.cpp` for
# `main.cpp` and Linux would not, and the check has to give the same answer on both.
function Resolve-TreePath([string]$rel) {
  $current = $Root
  foreach ($part in ($rel -split '/')) {
    if (-not $part) { continue }
    if (-not [IO.Directory]::Exists($current)) { throw "$rel does not exist" }
    $names = @([IO.Directory]::EnumerateFileSystemEntries($current) | ForEach-Object { [IO.Path]::GetFileName($_) })
    $exact = @($names | Where-Object { $_ -ceq $part })
    if ($exact.Count -eq 0) {
      $other = @($names | Where-Object { $_ -eq $part })
      if ($other.Count -gt 0) { throw "$rel does not exist: the tree spells '$part' as '$($other[0])'" }
      throw "$rel does not exist"
    }
    $current = Join-Path $current $exact[0]
  }
  return $current
}

# A file's text with its line endings made LF, so a checkout with CRLF counts what one with LF does.
function Get-Text([string]$path) { return ([IO.File]::ReadAllText($path) -replace "`r`n", "`n") }
function Get-Lines([string]$path) {
  $text = Get-Text $path
  if ($text.EndsWith("`n")) { $text = $text.Substring(0, $text.Length - 1) }
  if (-not $text) { return @() }
  return @($text -split "`n")
}

# One function's definition: the first line at column 0 that names it followed by `(` and is not a
# declaration (it does not end in `;`), through the first line after it that is `}` alone — the tree
# is clang-formatted, so a definition at namespace scope closes at column 0 and nothing inside it does.
function Get-FunctionLines([string]$path, [string]$name) {
  $lines = Get-Lines $path
  $pattern = '^\S.*(?<![\w:~])' + [regex]::Escape($name) + '\s*\('
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match $pattern -and $lines[$i] -notmatch ';\s*$') {
      for ($j = $i + 1; $j -lt $lines.Count; $j++) {
        if ($lines[$j] -ceq '}') { return , @($lines[$i..$j]) }
      }
      throw "$(Get-Rel $path): the definition of $name at line $($i + 1) has no closing '}' at column 0"
    }
  }
  throw "$(Get-Rel $path) has no definition of $name"
}

# Every page of the tree, sorted, by its tree path.
function Get-Pages {
  $found = New-Object System.Collections.Generic.List[string]
  $pending = New-Object System.Collections.Generic.Stack[string]
  $pending.Push($Root)
  while ($pending.Count -gt 0) {
    $dir = $pending.Pop()
    foreach ($sub in [IO.Directory]::EnumerateDirectories($dir)) {
      if ($excludedDirs -contains [IO.Path]::GetFileName($sub)) { continue }
      if ($excludedPaths -contains (Get-Rel $sub)) { continue }
      $pending.Push($sub)
    }
    foreach ($file in [IO.Directory]::EnumerateFiles($dir, '*.md')) { $found.Add((Get-Rel $file)) }
  }
  return @($found | Sort-Object { $_ } -CaseSensitive)
}

# Bytes with every CRLF counted as the LF it is in the repository.
function Get-PageBytes([string]$path) {
  $bytes = [IO.File]::ReadAllBytes($path)
  # Latin-1 maps each byte to one char, so the string's CRLFs are the file's.
  $text = [Text.Encoding]::Latin1.GetString($bytes)
  return $bytes.Length - ($text.Length - $text.Replace("`r`n", "`n").Length)
}

function Measure-Hub($hub) {
  switch ($hub.measure) {
    'file-lines' { return (Get-Lines (Resolve-TreePath $hub.file)).Count }
    'page-bytes' { return Get-PageBytes (Resolve-TreePath $hub.file) }
    'function-lines' { return (Get-FunctionLines (Resolve-TreePath $hub.file) $hub.function).Count }
    'function-matches' {
      $n = 0
      foreach ($line in (Get-FunctionLines (Resolve-TreePath $hub.file) $hub.function)) { $n += [regex]::Matches($line, $hub.pattern).Count }
      return $n
    }
    'schema-fields' {
      # The lines of `struct <name> ... {` through its `}` at column 0 that declare a field,
      # `name: type`; `///` comments and blank lines are not fields.
      $lines = Get-Lines (Resolve-TreePath $hub.file)
      $start = -1
      for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -cmatch ('^struct\s+' + [regex]::Escape($hub.struct) + '\b')) { $start = $i; break } }
      if ($start -lt 0) { throw "$($hub.file) has no struct $($hub.struct)" }
      $n = 0
      for ($i = $start + 1; $i -lt $lines.Count -and $lines[$i] -cne '}'; $i++) {
        if ($lines[$i] -match '^\s+[A-Za-z_]\w*\s*:' -and $lines[$i] -notmatch '^\s*//') { $n++ }
      }
      return $n
    }
    'conditionals' {
      # `#if` lines (not `#ifdef`, `#ifndef` or `#elif`: the tree's capability macros are 0 or 1 and
      # tested with `#if`) whose condition names a macro starting with the prefix, in the file or
      # in every source file under the directory.
      $target = Resolve-TreePath $hub.path
      $files = if ([IO.File]::Exists($target)) { @($target) } else {
        @(Get-ChildItem -LiteralPath $target -Recurse -File | Where-Object { $sourceExtensions -contains $_.Extension } | ForEach-Object { $_.FullName })
      }
      $pattern = '^\s*#\s*if\s.*\b' + [regex]::Escape($hub.macro_prefix) + '\w'
      $n = 0
      foreach ($f in $files) { foreach ($line in (Get-Lines $f)) { if ($line -cmatch $pattern) { $n++ } } }
      return $n
    }
    'shader-modules' {
      # The sources every engine_shaders() call in the tree's CMakeLists.txt compiles, one SPIR-V
      # module each, whose file name matches the wildcard: what the build produces, not a count.
      $modules = New-Object System.Collections.Generic.HashSet[string]
      foreach ($layer in $layerDirs) {
        $dir = Join-Path $Root $layer
        if (-not [IO.Directory]::Exists($dir)) { continue }
        foreach ($manifest in (Get-ChildItem -LiteralPath $dir -Recurse -File -Filter 'CMakeLists.txt')) {
          $text = (Get-Lines $manifest.FullName | ForEach-Object { $_ -replace '#.*$', '' }) -join "`n"
          foreach ($call in [regex]::Matches($text, '\bengine_shaders\s*\(([^)]*)\)')) {
            foreach ($token in ($call.Groups[1].Value -split '\s+')) {
              if ($token -notlike '*.slang') { continue }
              if ([IO.Path]::GetFileName($token) -clike $hub.match) {
                [void]$modules.Add((Get-Rel (Join-Path $manifest.DirectoryName $token)))
              }
            }
          }
        }
      }
      return $modules.Count
    }
    default { throw "hub '$($hub.id)': unknown measure '$($hub.measure)'" }
  }
}

function Get-Where($hub) {
  if ($hub.measure -eq 'shader-modules') { return "engine_shaders() sources matching $($hub.match)" }
  if ($hub.measure -eq 'conditionals') { return $hub.path }
  if ($hub.measure -like 'function-*') { return "$($hub.function), $($hub.file)" }
  if ($hub.measure -eq 'schema-fields') { return "struct $($hub.struct), $($hub.file)" }
  return $hub.file
}

# ---- read --------------------------------------------------------------------------------------

try {
  $spec = Get-Content -LiteralPath $Ceilings -Raw | ConvertFrom-Json
} catch {
  Write-Host "structure-check: cannot read $Ceilings`: $($_.Exception.Message)" -ForegroundColor Red
  exit 2
}
if ($spec.format -ne 'engine.structure-ceilings.v1') {
  Write-Host "structure-check: $Ceilings is not an engine.structure-ceilings.v1 file" -ForegroundColor Red
  exit 2
}
$lowerAfter = [double]$spec.lower_after_percent
$pageDefault = [long]$spec.page_default_bytes

# ---- measure -----------------------------------------------------------------------------------

$problems = New-Object System.Collections.Generic.List[string]
$rows = New-Object System.Collections.Generic.List[object]
$named = New-Object System.Collections.Generic.HashSet[string]
$ids = New-Object System.Collections.Generic.HashSet[string]

foreach ($hub in $spec.hubs) {
  if (-not $ids.Add($hub.id)) { $problems.Add("$($hub.id): named twice in the ceilings file"); continue }
  if ($hub.measure -eq 'page-bytes') { [void]$named.Add($hub.file) }
  try {
    $value = [long](Measure-Hub $hub)
  } catch {
    $problems.Add("$($hub.id): $($_.Exception.Message). A hub that has gone is taken out of the ceilings file by hand, in the commit that removed it, which says so.")
    $rows.Add([pscustomobject]@{ Hub = $hub; Value = $null; State = 'missing' })
    continue
  }
  $ceiling = [long]$hub.ceiling
  $slack = [long][math]::Floor($ceiling * $lowerAfter / 100)
  $state = if ($value -gt $ceiling) { 'over' } elseif ($ceiling - $value -gt $slack) { 'slack' } else { 'ok' }
  $rows.Add([pscustomobject]@{ Hub = $hub; Value = $value; State = $state })
  $unit = $units[$hub.measure]
  if ($state -eq 'over') {
    $problems.Add("$($hub.id): $value $unit, over its ceiling of $ceiling ($(Get-Where $hub))")
  } elseif ($state -eq 'slack') {
    $problems.Add("$($hub.id): $value $unit, $($ceiling - $value) under its ceiling of $ceiling, more than the $lowerAfter% a ceiling may stand above its hub ($(Get-Where $hub)): lower it")
  }
}

$pages = @(Get-Pages)
$unnamed = New-Object System.Collections.Generic.List[object]
foreach ($page in $pages) {
  if ($named.Contains($page)) { continue }
  $bytes = Get-PageBytes (Join-Path $Root $page)
  if ($bytes -gt $pageDefault) {
    $unnamed.Add([pscustomobject]@{ Page = $page; Bytes = $bytes })
    $problems.Add("${page}: $bytes bytes, over the default page ceiling of $pageDefault (a page past it is named in the file, with its own ceiling)")
  }
}

# ---- rebaseline --------------------------------------------------------------------------------

if ($Rebaseline) {
  $missing = @($rows | Where-Object { $_.State -eq 'missing' })
  if ($missing.Count -gt 0) {
    Write-Host 'structure-check: not rebaselined, because a hub is not there:' -ForegroundColor Red
    foreach ($p in $problems) { if ($p -match '^[^:]+: .*A hub that has gone') { Write-Host "  $p" } }
    exit 1
  }
  $changes = New-Object System.Collections.Generic.List[object]
  foreach ($row in $rows) {
    if ($row.Value -ne [long]$row.Hub.ceiling) {
      $changes.Add([pscustomobject][ordered]@{ id = $row.Hub.id; old = [long]$row.Hub.ceiling; new = $row.Value })
    }
  }
  foreach ($u in $unnamed) {
    $changes.Add([pscustomobject][ordered]@{ id = "page:$($u.Page)"; old = $pageDefault; new = $u.Bytes })
  }
  $raises = @($changes | Where-Object { $_.new -gt $_.old })
  if ($changes.Count -eq 0) {
    Write-Host 'structure-check: every ceiling is its hub''s value already; nothing to rebaseline'
    exit 0
  }
  if ($raises.Count -gt 0 -and -not $Reason.Trim()) {
    Write-Host 'structure-check: not rebaselined: these ceilings would go up, and a ceiling that goes up says why (-Reason "<why>"):' -ForegroundColor Red
    foreach ($c in $raises) { Write-Host "  $($c.id): $($c.old) -> $($c.new)" }
    exit 1
  }
  foreach ($row in $rows) { $row.Hub.ceiling = $row.Value }
  $hubs = New-Object System.Collections.Generic.List[object]
  foreach ($h in $spec.hubs) { $hubs.Add($h) }
  foreach ($u in $unnamed) {
    $hubs.Add([pscustomobject][ordered]@{ id = "page:$($u.Page)"; measure = 'page-bytes'; file = $u.Page; ceiling = $u.Bytes })
  }
  $spec.hubs = $hubs.ToArray()
  if ($Reason.Trim()) {
    if (-not $By) { $By = (& git -C $Root config user.name 2>$null) }
    $commit = (& git -C $Root rev-parse --short HEAD 2>$null)
    $record = [pscustomobject][ordered]@{ date = $Date; by = "$By"; reason = $Reason.Trim(); commit = "$commit"; changes = $changes.ToArray() }
    $spec.rebaselines = @($spec.rebaselines) + @($record)
  }
  $json = ($spec | ConvertTo-Json -Depth 8) -replace "`r`n", "`n"
  [IO.File]::WriteAllText($Ceilings, $json + "`n", (New-Object System.Text.UTF8Encoding $false))
  Write-Host "structure-check: rebaselined $($changes.Count) ceiling(s) in $(Get-Rel $Ceilings):"
  foreach ($c in $changes) { Write-Host ("  {0}: {1} -> {2}{3}" -f $c.id, $c.old, $c.new, $(if ($c.new -gt $c.old) { ' (raised)' } else { '' })) }
  Write-Host 'Commit the file with the change that moved them; a raise says why in the commit message too.'
  exit 0
}

# ---- report ------------------------------------------------------------------------------------

foreach ($row in $rows) {
  if ($null -eq $row.Value) { continue }
  Write-Host ("  {0,-44} {1,9} / {2,9} {3}" -f $row.Hub.id, $row.Value, $row.Hub.ceiling, $units[$row.Hub.measure])
}
if ($problems.Count -gt 0) {
  Write-Host "structure-check: $($problems.Count) problem(s)" -ForegroundColor Red
  foreach ($p in $problems) { Write-Host "  $p" }
  Write-Host ''
  Write-Host 'A hub over its ceiling grew: shrink it — split the function, move the section, give the'
  Write-Host 'feature a registration point of its own — or, if the growth is the change''s point, raise'
  Write-Host 'the ceiling with  pwsh tools/structure-check.ps1 -Rebaseline -Reason "<why>"  and say so in'
  Write-Host 'the commit message. A hub that shrank lowers its ceiling with  pwsh tools/structure-check.ps1'
  Write-Host '-Rebaseline  (no reason needed). Never edit a ceiling up by hand.'
  exit 1
}
Write-Host ("structure-check: OK ({0} hubs within their ceilings, {1} pages within the default of {2} bytes)" -f `
  $rows.Count, ($pages.Count - $named.Count), $pageDefault)
exit 0
