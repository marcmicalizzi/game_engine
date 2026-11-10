#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Ceilings for structure: the size of the files every feature edits, pinned the way the size
  tables pin a hot type's bytes and the frame budgets a pass's milliseconds.

.DESCRIPTION
  tools/structure-check.ps1 [-Root <tree>] [-Ceilings <file>] [-Raises <dir>]
  tools/structure-check.ps1 -Raise <name> [-Reason "<why>"] [-By <who>]     # an author, on a branch
  tools/structure-check.ps1 -Rebaseline [-Reason "<why>"] [-By <who>]       # the coordinator, after a merge

  Reads tools/structure-ceilings.json, measures every hub it names in the tree — the lines of one
  function, the lines of a file, the shader modules the build compiles, the fields of a schema
  struct, the `#if` lines that name a capability's macro, the bytes of a page — and every page the
  file neither names nor lists as unpinned against its default, and fails naming each value over
  its effective ceiling.

  A hub's **effective ceiling** is its ceiling in the ceilings file plus the sum of the signed
  deltas every file in tools/structure-raises/ gives its id (a page past the default is
  `page:<path>`, on top of the default). A value more than `lower_after_percent` under its
  effective ceiling fails too, so the slack a shrinking hub leaves cannot let the growth back.

  An author never edits the ceilings file. -Raise <name> writes or updates the branch's own file,
  tools/structure-raises/<name>.json, from what the tree measures now: each hub's delta is what
  the tree needs beyond the ceiling and every other file, run twice it accumulates into the same
  file, and an entry back at zero is dropped (the file with it, when it is the last). A file with a
  positive delta refuses without -Reason; one of negative deltas only needs none. Two branches
  that each grow a hub, each with its own file, merge with no edit and no conflict: their sizes add
  and so do their deltas.

  -Rebaseline is the fold, and the coordinator's, after a merge: every ceiling becomes its measured
  value, each raise file's name, reason and deltas move into the ceilings file's `rebaselines`, and
  the files are deleted. It needs no reason when the files cover every raise, and refuses without
  -Reason while any value is over its effective ceiling.

  How each measure is counted, and why, is on the rule's page (docs/ci/structure-and-citations.md,
  "Ceilings for structure"). Runs under CTest as `structure_check`;
  tools/structure-check.Tests.ps1 is its own test (`tools.structure_check`).

  Exit codes: 0 every value within its effective ceiling (or the file written); 1 a value over or
  too far under, a hub that is not there, a raise file that is wrong, or a refused write; 2 a
  ceilings file that cannot be read.

.EXAMPLE
  pwsh tools/structure-check.ps1
  pwsh tools/structure-check.ps1 -Raise T81 -Reason "the row's own bullet in AGENTS.md"
  pwsh tools/structure-check.ps1 -Rebaseline
#>
[CmdletBinding()]
param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
  [string]$Ceilings = '',
  [string]$Raises = '',
  [string]$Raise = '',
  [switch]$Rebaseline,
  [string]$Reason = '',
  [string]$By = '',
  [string]$Date = (Get-Date -Format 'yyyy-MM-dd')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path -LiteralPath $Root).Path
if (-not $Ceilings) { $Ceilings = Join-Path $Root 'tools/structure-ceilings.json' }
if (-not $Raises) { $Raises = Join-Path (Split-Path -Parent $Ceilings) 'structure-raises' }
if ($Raise -and $Rebaseline) { Write-Host 'structure-check: -Raise and -Rebaseline are two commands; give one' -ForegroundColor Red; exit 1 }

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

function Get-Prop($object, [string]$name) {
  $p = $object.PSObject.Properties[$name]
  if ($p) { return $p.Value } else { return $null }
}

function Write-Json([string]$path, $value) {
  $json = ($value | ConvertTo-Json -Depth 10) -replace "`r`n", "`n"
  [IO.File]::WriteAllText($path, $json + "`n", (New-Object System.Text.UTF8Encoding $false))
}

$raiseNameRegex = '^[A-Za-z0-9][A-Za-z0-9._-]*$'

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

# Pages that are neither pinned nor held to the default, each with its reason (a ledger that grows
# a row per piece of work, whose diet is archiving, not a ceiling).
$unpinned = New-Object System.Collections.Generic.HashSet[string]
foreach ($u in @(Get-Prop $spec 'unpinned_pages')) { if ($u) { [void]$unpinned.Add([string]$u.file) } }

$problems = New-Object System.Collections.Generic.List[string]

# The raise files: every `<name>.json` of the directory, its name compared ordinally, the way hub
# paths are, so a checkout on Windows and one on Linux read the same set.
$raiseFiles = New-Object System.Collections.Generic.List[object]
$deltaSum = @{}
if ([IO.Directory]::Exists($Raises)) {
  $entries = @([IO.Directory]::EnumerateFiles($Raises) | ForEach-Object { [IO.Path]::GetFileName($_) } |
    Where-Object { $_.EndsWith('.json', [StringComparison]::Ordinal) } | Sort-Object { $_ } -CaseSensitive)
  foreach ($fileName in $entries) {
    $name = $fileName.Substring(0, $fileName.Length - 5)
    $path = Join-Path $Raises $fileName
    $label = "structure-raises/$fileName"
    try { $data = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } catch { $problems.Add("${label}: not JSON ($($_.Exception.Message))"); continue }
    if ((Get-Prop $data 'format') -ne 'engine.structure-raise.v1') { $problems.Add("${label}: not an engine.structure-raise.v1 file"); continue }
    if ($name -notmatch $raiseNameRegex) { $problems.Add("${label}: '$name' is not a raise name (letters, digits, '.', '_', '-')"); continue }
    $deltas = [ordered]@{}
    $deltaObject = Get-Prop $data 'deltas'
    if ($deltaObject) { foreach ($p in $deltaObject.PSObject.Properties) { $deltas[$p.Name] = [long]$p.Value } }
    $fileReason = [string](Get-Prop $data 'reason')
    $positive = @($deltas.Keys | Where-Object { $deltas[$_] -gt 0 })
    if ($positive.Count -gt 0 -and -not $fileReason.Trim()) {
      $problems.Add("${label}: raises $($positive -join ', ') and gives no reason; a ceiling that goes up says why")
    }
    foreach ($id in $deltas.Keys) { $deltaSum[$id] = [long]$(if ($deltaSum.ContainsKey($id)) { $deltaSum[$id] } else { 0 }) + $deltas[$id] }
    $raiseFiles.Add([pscustomobject]@{ Name = $name; FileName = $fileName; Path = $path; Data = $data; Deltas = $deltas; Reason = $fileReason })
  }
}
function Get-Delta([string]$id, [string]$except = '') {
  $sum = 0L
  foreach ($f in $raiseFiles) { if ($f.Name -cne $except -and $f.Deltas.Contains($id)) { $sum += $f.Deltas[$id] } }
  return $sum
}

# ---- measure -----------------------------------------------------------------------------------

$rows = New-Object System.Collections.Generic.List[object]
$named = New-Object System.Collections.Generic.HashSet[string]
$ids = New-Object System.Collections.Generic.HashSet[string]

foreach ($hub in $spec.hubs) {
  if (-not $ids.Add($hub.id)) { $problems.Add("$($hub.id): named twice in the ceilings file"); continue }
  if ($hub.measure -eq 'page-bytes') { [void]$named.Add($hub.file) }
  $delta = Get-Delta $hub.id
  $effective = [long]$hub.ceiling + $delta
  try {
    $value = [long](Measure-Hub $hub)
  } catch {
    $problems.Add("$($hub.id): $($_.Exception.Message). A hub that has gone is taken out of the ceilings file by hand, in the commit that removed it, which says so.")
    $rows.Add([pscustomobject]@{ Hub = $hub; Value = $null; Delta = $delta; Effective = $effective; State = 'missing' })
    continue
  }
  $slack = [long][math]::Floor($effective * $lowerAfter / 100)
  $state = if ($value -gt $effective) { 'over' } elseif ($effective - $value -gt $slack) { 'slack' } else { 'ok' }
  $rows.Add([pscustomobject]@{ Hub = $hub; Value = $value; Delta = $delta; Effective = $effective; State = $state })
  $unit = $units[$hub.measure]
  $of = if ($delta -ne 0) { "$effective (the file's $($hub.ceiling), raises $('{0:+#;-#;0}' -f $delta))" } else { "$effective" }
  if ($state -eq 'over') {
    $problems.Add("$($hub.id): $value $unit, over its ceiling of $of ($(Get-Where $hub))")
  } elseif ($state -eq 'slack') {
    $problems.Add("$($hub.id): $value $unit, $($effective - $value) under its ceiling of $of, more than the $lowerAfter% a ceiling may stand above its hub ($(Get-Where $hub)): record the drop")
  }
}

$pages = @(Get-Pages)
$unnamed = New-Object System.Collections.Generic.List[object]
foreach ($page in $pages) {
  if ($named.Contains($page) -or $unpinned.Contains($page)) { continue }
  $bytes = Get-PageBytes (Join-Path $Root $page)
  $id = "page:$page"
  $effective = $pageDefault + (Get-Delta $id)
  $unnamed.Add([pscustomobject]@{ Page = $page; Id = $id; Bytes = $bytes; Effective = $effective })
  if ($bytes -gt $effective) {
    $problems.Add("${page}: $bytes bytes, over the default page ceiling of $effective (a page past the default carries a raise, and is named in the file at the next fold)")
  }
}

# A delta names a hub or a page held to the default; anything else is a typo that would hide.
$knownIds = New-Object System.Collections.Generic.HashSet[string]
foreach ($id in $ids) { [void]$knownIds.Add($id) }
foreach ($u in $unnamed) { [void]$knownIds.Add($u.Id) }
foreach ($f in $raiseFiles) {
  foreach ($id in $f.Deltas.Keys) {
    if (-not $knownIds.Contains($id)) { $problems.Add("structure-raises/$($f.FileName): '$id' names no hub of the ceilings file and no page held to the default") }
  }
}

$missing = @($rows | Where-Object { $_.State -eq 'missing' })

# ---- raise: the branch's own file ----------------------------------------------------------------

if ($Raise) {
  if ($Raise -notmatch $raiseNameRegex) { Write-Host "structure-check: '$Raise' is not a raise name (letters, digits, '.', '_', '-'; the row, or a short name)" -ForegroundColor Red; exit 1 }
  if ($missing.Count -gt 0) { Write-Host 'structure-check: no raise written, because a hub is not there:' -ForegroundColor Red; foreach ($r in $missing) { Write-Host "  $($r.Hub.id)" }; exit 1 }
  # The name as the directory spells it, ordinally: on Windows `t81.json` would open `T81.json`.
  if ([IO.Directory]::Exists($Raises)) {
    $other = @([IO.Directory]::EnumerateFiles($Raises) | ForEach-Object { [IO.Path]::GetFileName($_) } |
      Where-Object { $_ -ieq "$Raise.json" -and $_ -cne "$Raise.json" })
    if ($other.Count -gt 0) { Write-Host "structure-check: no raise written: the directory spells '$Raise.json' as '$($other[0])'; use that name" -ForegroundColor Red; exit 1 }
  }
  $mine = $raiseFiles | Where-Object { $_.Name -ceq $Raise } | Select-Object -First 1
  $deltas = [ordered]@{}
  foreach ($row in $rows) {
    $d = $row.Value - [long]$row.Hub.ceiling - (Get-Delta $row.Hub.id $Raise)
    if ($d -ne 0) { $deltas[$row.Hub.id] = $d }
  }
  foreach ($u in $unnamed) {
    $d = $u.Bytes - $pageDefault - (Get-Delta $u.Id $Raise)
    if ($d -gt 0) { $deltas[$u.Id] = $d }
  }
  $old = if ($mine) { $mine.Deltas } else { [ordered]@{} }
  $fileReason = if ($Reason.Trim()) { $Reason.Trim() } elseif ($mine) { $mine.Reason } else { '' }
  $positive = @($deltas.Keys | Where-Object { $deltas[$_] -gt 0 })
  if ($positive.Count -gt 0 -and -not $fileReason.Trim()) {
    Write-Host "structure-check: no raise written: these hubs grow, and a ceiling that goes up says why (-Reason `"<why>`"):" -ForegroundColor Red
    foreach ($id in $positive) { Write-Host "  ${id}: +$($deltas[$id])" }
    exit 1
  }
  $path = Join-Path $Raises "$Raise.json"
  if ($deltas.Count -eq 0) {
    if ($mine) { Remove-Item -LiteralPath $mine.Path; Write-Host "structure-check: every hub is back at its ceiling; removed structure-raises/$Raise.json" }
    else { Write-Host 'structure-check: every hub is at its ceiling; no raise to write' }
    exit 0
  }
  if (-not $By) { $By = (& git -C $Root config user.name 2>$null) }
  New-Item -ItemType Directory -Force -Path $Raises | Out-Null
  Write-Json $path ([ordered]@{ format = 'engine.structure-raise.v1'; name = $Raise; reason = $fileReason; date = $Date; by = "$By"; deltas = $deltas })
  Write-Host "structure-check: wrote structure-raises/$Raise.json:"
  foreach ($id in $deltas.Keys) {
    $was = if ($old.Contains($id)) { $old[$id] } else { 0 }
    Write-Host ("  {0}: {1:+#;-#;0}{2}" -f $id, $deltas[$id], $(if ($was -ne $deltas[$id]) { " (was $('{0:+#;-#;0}' -f $was))" } else { '' }))
  }
  foreach ($id in $old.Keys) { if (-not $deltas.Contains($id)) { Write-Host "  ${id}: dropped, back at its ceiling" } }
  Write-Host 'Commit the file with the change; a raise says why in the commit message too.'
  exit 0
}

# ---- rebaseline: the fold ------------------------------------------------------------------------

if ($Rebaseline) {
  if ($missing.Count -gt 0) {
    Write-Host 'structure-check: not rebaselined, because a hub is not there:' -ForegroundColor Red
    foreach ($r in $missing) { Write-Host "  $($r.Hub.id)" }
    exit 1
  }
  $bad = @($problems | Where-Object { $_ -like 'structure-raises/*' })
  if ($bad.Count -gt 0) { Write-Host 'structure-check: not rebaselined, because a raise file is wrong:' -ForegroundColor Red; foreach ($p in $bad) { Write-Host "  $p" }; exit 1 }
  $over = New-Object System.Collections.Generic.List[string]
  foreach ($row in $rows) { if ($row.Value -gt $row.Effective) { $over.Add("$($row.Hub.id): $($row.Effective) -> $($row.Value)") } }
  foreach ($u in $unnamed) { if ($u.Bytes -gt $u.Effective) { $over.Add("$($u.Id): $($u.Effective) -> $($u.Bytes)") } }
  if ($over.Count -gt 0 -and -not $Reason.Trim()) {
    Write-Host 'structure-check: not rebaselined: these ceilings would go up past every raise file, and a ceiling that goes up says why (-Reason "<why>"):' -ForegroundColor Red
    foreach ($o in $over) { Write-Host "  $o" }
    exit 1
  }
  $changes = New-Object System.Collections.Generic.List[object]
  foreach ($row in $rows) {
    if ($row.Value -ne [long]$row.Hub.ceiling) { $changes.Add([pscustomobject][ordered]@{ id = $row.Hub.id; old = [long]$row.Hub.ceiling; new = $row.Value }) }
    $row.Hub.ceiling = $row.Value
  }
  $hubs = New-Object System.Collections.Generic.List[object]
  foreach ($h in $spec.hubs) { $hubs.Add($h) }
  foreach ($u in $unnamed) {
    if ($u.Bytes -le $pageDefault) { continue }
    $changes.Add([pscustomobject][ordered]@{ id = $u.Id; old = $pageDefault; new = $u.Bytes })
    $hubs.Add([pscustomobject][ordered]@{ id = $u.Id; measure = 'page-bytes'; file = $u.Page; ceiling = $u.Bytes })
  }
  $spec.hubs = $hubs.ToArray()
  if ($changes.Count -eq 0 -and $raiseFiles.Count -eq 0) {
    Write-Host 'structure-check: every ceiling is its hub''s value already and there is no raise file; nothing to rebaseline'
    exit 0
  }
  if ($raiseFiles.Count -gt 0 -or $Reason.Trim()) {
    if (-not $By) { $By = (& git -C $Root config user.name 2>$null) }
    $commit = (& git -C $Root rev-parse --short HEAD 2>$null)
    $folded = @($raiseFiles | ForEach-Object {
      [pscustomobject][ordered]@{ name = $_.Name; reason = $_.Reason; date = [string](Get-Prop $_.Data 'date'); by = [string](Get-Prop $_.Data 'by'); deltas = [pscustomobject]$_.Deltas }
    })
    $record = [pscustomobject][ordered]@{ date = $Date; by = "$By"; reason = $Reason.Trim(); commit = "$commit"; folded = $folded; changes = $changes.ToArray() }
    $spec.rebaselines = @(@(Get-Prop $spec 'rebaselines') | Where-Object { $_ }) + @($record)
  }
  Write-Json $Ceilings $spec
  foreach ($f in $raiseFiles) { Remove-Item -LiteralPath $f.Path }
  Write-Host "structure-check: rebaselined $($changes.Count) ceiling(s) in $(Get-Rel $Ceilings), folding $($raiseFiles.Count) raise file(s):"
  foreach ($c in $changes) { Write-Host ("  {0}: {1} -> {2}" -f $c.id, $c.old, $c.new) }
  foreach ($f in $raiseFiles) { Write-Host "  folded and removed structure-raises/$($f.FileName)" }
  Write-Host 'Commit the ceilings file and the removed raise files together.'
  exit 0
}

# ---- report ------------------------------------------------------------------------------------

foreach ($row in $rows) {
  if ($null -eq $row.Value) { continue }
  $raised = if ($row.Delta -ne 0) { " (raises $('{0:+#;-#;0}' -f $row.Delta))" } else { '' }
  Write-Host ("  {0,-44} {1,9} / {2,9} {3}{4}" -f $row.Hub.id, $row.Value, $row.Effective, $units[$row.Hub.measure], $raised)
}
foreach ($u in $unnamed) { if ($u.Effective -ne $pageDefault) { Write-Host ("  {0,-44} {1,9} / {2,9} bytes (raises +{3})" -f $u.Id, $u.Bytes, $u.Effective, ($u.Effective - $pageDefault)) } }
if ($problems.Count -gt 0) {
  Write-Host "structure-check: $($problems.Count) problem(s)" -ForegroundColor Red
  foreach ($p in $problems) { Write-Host "  $p" }
  Write-Host ''
  Write-Host 'A hub over its ceiling grew: shrink it — split the function, move the section, give the'
  Write-Host 'feature a registration point of its own — or, if the growth is the change''s point, record it'
  Write-Host 'in a file of the branch''s own with'
  Write-Host '    pwsh tools/structure-check.ps1 -Raise <row> -Reason "<why>"'
  Write-Host 'and commit tools/structure-raises/<row>.json, saying why in the commit message. A hub that'
  Write-Host 'shrank is recorded the same way, with no reason needed. Never edit tools/structure-ceilings.json:'
  Write-Host 'the coordinator folds the raise files into it after a merge (-Rebaseline).'
  exit 1
}
Write-Host ("structure-check: OK ({0} hubs within their ceilings, {1} raise file(s), {2} pages within the default of {3} bytes, {4} unpinned)" -f `
  $rows.Count, $raiseFiles.Count, $unnamed.Count, $pageDefault, $unpinned.Count)
exit 0
