#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/structure-check.ps1. Runs under CTest as `tools.structure_check`.

.DESCRIPTION
  Builds a small tree in a temporary directory — a function, a file, a CMakeLists.txt with
  engine_shaders(), a schema struct, `#if` lines and pages — with a ceilings file of its exact
  values, checks that it passes, and then moves one value at a time: over its ceiling, under it by
  less and by more than the slack, a rebaseline that raises with and without a reason, one that
  only lowers, the same files with CRLF line endings, and a hub that is not there. Each failing
  case asserts the message *and* the exit code, because a check that reports a problem and exits
  0 is worse than no check at all.

      pwsh tools/structure-check.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'

$check = Join-Path $PSScriptRoot 'structure-check.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$roots = New-Object System.Collections.Generic.List[string]

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

function Invoke-Check([string]$root, [hashtable]$extra = @{}) {
  $out = (& $check -Root $root -Ceilings (Join-Path $root 'ceilings.json') @extra *>&1 | Out-String)
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

function Test-Reports([string]$what, [string]$root, [string]$expected, [hashtable]$extra = @{}, [int]$code = 1) {
  $script:checks++
  $result = Invoke-Check $root $extra
  if ($result.Code -eq $code -and $result.Out.Contains($expected)) {
    Write-Host "  ok   $what"
  } else {
    Write-Host "  FAIL $what (exit $($result.Code), expected $code and to see: $expected)" -ForegroundColor Red
    Write-Host ($result.Out -split "`n" | ForEach-Object { "       $_" }) -Separator "`n"
    $failures.Add($what)
  }
  return $result
}

function Set-FixtureFile([string]$root, [string]$rel, [string]$text, [string]$eol = "`n") {
  $path = Join-Path $root $rel
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
  $text = ($text -replace "`r`n", "`n") -replace "`n", $eol
  [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

function Get-Ceilings([string]$root) { return (Get-Content -LiteralPath (Join-Path $root 'ceilings.json') -Raw | ConvertFrom-Json) }
function Set-Ceilings([string]$root, $spec) {
  [IO.File]::WriteAllText((Join-Path $root 'ceilings.json'), ($spec | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding $false))
}
function Set-Ceiling([string]$root, [string]$id, [long]$value) {
  $spec = Get-Ceilings $root
  ($spec.hubs | Where-Object { $_.id -eq $id }).ceiling = $value
  Set-Ceilings $root $spec
}
function Get-Ceiling([string]$root, [string]$id) { return [long]((Get-Ceilings $root).hubs | Where-Object { $_.id -eq $id }).ceiling }

# The fixture's measured values: frame 7 lines with 2 add_pass, r.cpp 13 lines, 2 resolve modules,
# 3 fields, 2 `#if` lines, big.md 300 bytes. small.md is 50 bytes, under the default of 200.
function New-Fixture([string]$eol = "`n") {
  $root = Join-Path ([IO.Path]::GetTempPath()) "engine-structure-check-$([guid]::NewGuid().ToString('N'))"
  $roots.Add($root)
  Set-FixtureFile $root 'systems/renderer/src/r.cpp' @'
int helper();
bool Renderer::frame(int a);

bool Renderer::frame(int a,
                     int b) {
  graph.add_pass("a");
  if (a) {
    graph.add_pass<X>("b");
  }
}

void other() {}
// end
'@ $eol
  Set-FixtureFile $root 'apps/view/main.cpp' @'
#if ENGINE_VIEW_A
#endif
#ifdef ENGINE_VIEW_B
#endif
  #  if defined(ENGINE_VIEW_C) && ENGINE_VIEW_D
#endif
#if ENGINE_HOST_A
#endif
'@ $eol
  Set-FixtureFile $root 'domain/gfx/CMakeLists.txt' @'
engine_shaders(NAME gfx SOURCES shaders/a.slang shaders/visibility_resolve.slang
               # shaders/visibility_resolve_old.slang
               shaders/visibility_resolve_rt.slang)
engine_module(NAME gfx LAYER domain)
'@ $eol
  Set-FixtureFile $root 'schemas/scene.schema' @'
struct Other @version(1) {
  x: f32
}

/// The root.
struct Scene @version(2) {
  /// The format.
  format: string
  meshes: Mesh[]

  sky: Sky? @since(2)
}
'@ $eol
  Set-FixtureFile $root 'docs/big.md' ("# Big`n" + ('x' * 293) + "`n") $eol
  Set-FixtureFile $root 'docs/small.md' ("# Small`n" + ('y' * 41) + "`n") $eol
  Set-FixtureFile $root 'build/huge.md' ('z' * 1000) $eol
  Set-FixtureFile $root 'docs/ledger.md' ('l' * 5000) $eol
  $spec = [pscustomobject][ordered]@{
    format = 'engine.structure-ceilings.v1'; lower_after_percent = 10; page_default_bytes = 200
    unpinned_pages = @([pscustomobject][ordered]@{ file = 'docs/ledger.md'; why = 'a ledger' })
    hubs = @(
      [pscustomobject][ordered]@{ id = 'frame.lines'; measure = 'function-lines'; file = 'systems/renderer/src/r.cpp'; function = 'Renderer::frame'; ceiling = 7 }
      [pscustomobject][ordered]@{ id = 'frame.add_pass'; measure = 'function-matches'; file = 'systems/renderer/src/r.cpp'; function = 'Renderer::frame'; pattern = '\badd_pass\s*[(<]'; ceiling = 2 }
      [pscustomobject][ordered]@{ id = 'r.lines'; measure = 'file-lines'; file = 'systems/renderer/src/r.cpp'; ceiling = 13 }
      [pscustomobject][ordered]@{ id = 'resolve'; measure = 'shader-modules'; match = 'visibility_resolve*.slang'; ceiling = 2 }
      [pscustomobject][ordered]@{ id = 'scene.fields'; measure = 'schema-fields'; file = 'schemas/scene.schema'; struct = 'Scene'; ceiling = 3 }
      [pscustomobject][ordered]@{ id = 'view.if'; measure = 'conditionals'; path = 'apps/view'; macro_prefix = 'ENGINE_VIEW_'; ceiling = 2 }
      [pscustomobject][ordered]@{ id = 'page:docs/big.md'; measure = 'page-bytes'; file = 'docs/big.md'; ceiling = 300 }
    )
    rebaselines = @()
  }
  Set-Ceilings $root $spec
  return $root
}

try {
  Write-Host 'case: a tree at its ceilings'
  $root = New-Fixture
  $r = Invoke-Check $root
  Test-That 'passes as built' { $r.Code -eq 0 -and $r.Out.Contains('structure-check: OK (7 hubs') }
  Test-That 'counts what the page says it counts' {
    $r.Out -match 'frame\.lines\s+7 /' -and $r.Out -match 'frame\.add_pass\s+2 /' -and $r.Out -match 'r\.lines\s+13 /' -and
    $r.Out -match 'resolve\s+2 /' -and $r.Out -match 'scene\.fields\s+3 /' -and $r.Out -match 'view\.if\s+2 /' -and
    $r.Out -match 'page:docs/big\.md\s+300 /'
  }
  Test-That 'a page under build/ is not the tree''s' { -not $r.Out.Contains('huge.md') }

  Write-Host 'case: the same files with CRLF line endings'
  $root = New-Fixture "`r`n"
  $r = Invoke-Check $root
  Test-That 'count what the LF files count' { $r.Code -eq 0 -and $r.Out -match 'frame\.lines\s+7 /' -and $r.Out -match 'r\.lines\s+13 /' -and $r.Out -match 'page:docs/big\.md\s+300 /' }

  Write-Host 'case: over and under'
  $root = New-Fixture
  Set-Ceiling $root 'frame.lines' 6
  [void](Test-Reports 'a value over its ceiling fails and names it' $root 'frame.lines: 7 lines, over its ceiling of 6 (Renderer::frame, systems/renderer/src/r.cpp)')
  $root = New-Fixture
  Set-Ceiling $root 'r.lines' 14
  Test-That 'a value under its ceiling by no more than the slack passes' { (Invoke-Check $root).Code -eq 0 }
  $root = New-Fixture
  Set-Ceiling $root 'r.lines' 16
  [void](Test-Reports 'a value under its ceiling by more than the slack fails: lower it' $root 'r.lines: 13 lines, 3 under its ceiling of 16, more than the 10% a ceiling may stand above its hub')
  $root = New-Fixture
  Set-Ceiling $root 'resolve' 3
  [void](Test-Reports 'a count with no slack in it fails when it drops by one' $root 'resolve: 2 modules, 1 under its ceiling of 3')
  $root = New-Fixture
  Set-FixtureFile $root 'docs/small.md' ('y' * 201)
  [void](Test-Reports 'a page the file does not name, past the default' $root 'docs/small.md: 201 bytes, over the default page ceiling of 200')

  Write-Host 'case: a hub that is not there is an error'
  $root = New-Fixture
  Remove-Item -LiteralPath (Join-Path $root 'systems/renderer/src/r.cpp')
  $r = Test-Reports 'a missing file' $root 'frame.lines: systems/renderer/src/r.cpp does not exist'
  Test-That 'and the run says no OK' { -not $r.Out.Contains('structure-check: OK') }
  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/src/r.cpp' "int main() {`n}`n"
  [void](Test-Reports 'a function that is not defined' $root 'frame.lines: systems/renderer/src/r.cpp has no definition of Renderer::frame')
  $root = New-Fixture
  $spec = Get-Ceilings $root
  ($spec.hubs | Where-Object { $_.id -eq 'r.lines' }).file = 'systems/renderer/src/R.cpp'
  Set-Ceilings $root $spec
  [void](Test-Reports 'a path in another case, on every host' $root "r.lines: systems/renderer/src/R.cpp does not exist: the tree spells 'R.cpp' as 'r.cpp'")
  $root = New-Fixture
  Set-FixtureFile $root 'schemas/scene.schema' "struct Other @version(1) {`n}`n"
  [void](Test-Reports 'a struct that is not in the schema' $root 'scene.fields: schemas/scene.schema has no struct Scene')
  $root = New-Fixture
  Set-FixtureFile $root 'ceilings.json' '{ "format": "something else" }'
  [void](Test-Reports 'a file that is not a ceilings file' $root 'is not an engine.structure-ceilings.v1 file' @{} 2)

  Write-Host 'case: rebaselines'
  $root = New-Fixture
  Set-Ceiling $root 'frame.lines' 5
  $before = Get-Content -LiteralPath (Join-Path $root 'ceilings.json') -Raw
  [void](Test-Reports 'a rebaseline that raises refuses without a reason' $root 'frame.lines: 5 -> 7' @{ Rebaseline = $true })
  Test-That 'and writes nothing' { (Get-Content -LiteralPath (Join-Path $root 'ceilings.json') -Raw) -eq $before }
  $r = Invoke-Check $root @{ Rebaseline = $true; Reason = 'the fixture grew'; By = 'tester'; Date = '2026-11-09' }
  $spec = Get-Ceilings $root
  Test-That 'with a reason it raises the ceiling' { $r.Code -eq 0 -and (Get-Ceiling $root 'frame.lines') -eq 7 }
  Test-That 'and keeps the reason, the date, who, and the old and new values' {
    $rec = @($spec.rebaselines)[0]
    @($spec.rebaselines).Count -eq 1 -and $rec.reason -eq 'the fixture grew' -and $rec.date -eq '2026-11-09' -and $rec.by -eq 'tester' -and
    @($rec.changes).Count -eq 1 -and $rec.changes[0].id -eq 'frame.lines' -and $rec.changes[0].old -eq 5 -and $rec.changes[0].new -eq 7
  }
  Test-That 'and the tree then passes' { (Invoke-Check $root).Code -eq 0 }

  $root = New-Fixture
  Set-Ceiling $root 'r.lines' 20
  $r = Invoke-Check $root @{ Rebaseline = $true }
  $spec = Get-Ceilings $root
  Test-That 'a rebaseline that only lowers needs no reason' { $r.Code -eq 0 -and (Get-Ceiling $root 'r.lines') -eq 13 }
  Test-That 'and keeps no record' { @($spec.rebaselines).Count -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'docs/small.md' ('y' * 250)
  [void](Test-Reports 'a page past the default is a raise' $root 'page:docs/small.md: 200 -> 250' @{ Rebaseline = $true })
  $r = Invoke-Check $root @{ Rebaseline = $true; Reason = 'small grew' }
  Test-That 'which, with a reason, names the page with its own ceiling' { $r.Code -eq 0 -and (Get-Ceiling $root 'page:docs/small.md') -eq 250 -and (Invoke-Check $root).Code -eq 0 }

  $root = New-Fixture
  [void](Test-Reports 'a rebaseline with nothing to move' $root 'nothing to rebaseline' @{ Rebaseline = $true } 0)
  $root = New-Fixture
  Remove-Item -LiteralPath (Join-Path $root 'schemas/scene.schema')
  [void](Test-Reports 'a rebaseline refuses while a hub is not there' $root 'not rebaselined, because a hub is not there' @{ Rebaseline = $true; Reason = 'x' })

  Write-Host 'case: an unpinned page'
  $root = New-Fixture
  $r = Invoke-Check $root
  Test-That 'is not measured: 5,000 bytes past a default of 200 and not reported' { $r.Code -eq 0 -and -not $r.Out.Contains('ledger') -and $r.Out.Contains('1 unpinned') }

  Write-Host 'case: raise files, and two branches that merge'
  # Branch A adds 2 lines to r.cpp and 10 bytes to big.md; branch B 3 lines and 20 bytes. The
  # merged tree has both edits and both files, which is what git gives it with no conflict.
  function Add-Growth([string]$root, [int]$lines, [int]$bytes, [string]$tag) {
    $cpp = Join-Path $root 'systems/renderer/src/r.cpp'
    $text = [IO.File]::ReadAllText($cpp); if (-not $text.EndsWith("`n")) { $text += "`n" }
    for ($k = 0; $k -lt $lines; $k++) { $text += "// $tag $k`n" }
    [IO.File]::WriteAllText($cpp, $text)
    $md = Join-Path $root 'docs/big.md'
    [IO.File]::WriteAllText($md, [IO.File]::ReadAllText($md) + ($tag.Substring(0, 1) * ($bytes - 1)) + "`n")
  }
  function Get-RaiseFile([string]$root, [string]$name) { return (Get-Content -LiteralPath (Join-Path $root "structure-raises/$name.json") -Raw | ConvertFrom-Json) }
  $base = New-Fixture
  Test-That 'the base passes' { (Invoke-Check $base).Code -eq 0 }
  $a = New-Fixture; Add-Growth $a 2 10 'alpha'
  [void](Test-Reports 'a branch that grows a hub fails, naming the command that records it' $a 'pwsh tools/structure-check.ps1 -Raise <row> -Reason "<why>"')
  [void](Test-Reports 'a raise that grows refuses without a reason' $a 'r.lines: +2' @{ Raise = 'A' })
  Test-That 'and writes nothing' { -not (Test-Path -LiteralPath (Join-Path $a 'structure-raises')) }
  $r = Invoke-Check $a @{ Raise = 'A'; Reason = 'alpha grows'; By = 'tester'; Date = '2026-11-10' }
  $fa = Get-RaiseFile $a 'A'
  Test-That 'with a reason it writes the branch''s own file: reason, date, who, a signed delta per hub' {
    $r.Code -eq 0 -and $fa.reason -eq 'alpha grows' -and $fa.date -eq '2026-11-10' -and $fa.by -eq 'tester' -and
    $fa.deltas.'r.lines' -eq 2 -and $fa.deltas.'page:docs/big.md' -eq 10 -and @($fa.deltas.PSObject.Properties).Count -eq 2
  }
  Test-That 'and the branch passes, the ceilings file untouched' { (Invoke-Check $a).Code -eq 0 -and (Get-Ceiling $a 'r.lines') -eq 13 }
  $b = New-Fixture; Add-Growth $b 3 20 'beta'
  [void](Invoke-Check $b @{ Raise = 'B'; Reason = 'beta grows' })
  Test-That 'the other branch alone passes with its own file' { (Invoke-Check $b).Code -eq 0 }
  $m = New-Fixture; Add-Growth $m 2 10 'alpha'; Add-Growth $m 3 20 'beta'
  New-Item -ItemType Directory -Force -Path (Join-Path $m 'structure-raises') | Out-Null
  Copy-Item -LiteralPath (Join-Path $a 'structure-raises/A.json') -Destination (Join-Path $m 'structure-raises/A.json')
  $r = Invoke-Check $m
  [void](Test-Reports 'the merge with one branch''s file missing fails, naming the hub' $m 'r.lines: 18 lines, over its ceiling of 15')
  Copy-Item -LiteralPath (Join-Path $b 'structure-raises/B.json') -Destination (Join-Path $m 'structure-raises/B.json')
  $r = Invoke-Check $m
  Test-That 'the two together pass with no edit: sizes add and so do deltas' { $r.Code -eq 0 -and $r.Out -match 'r\.lines\s+18 /\s+18 lines \(raises \+5\)' }

  Write-Host 'case: one branch''s file, run again'
  Add-Growth $a 1 1 'again'
  [void](Invoke-Check $a @{ Raise = 'A' })
  $fa = Get-RaiseFile $a 'A'
  Test-That 'accumulates into the same file, keeping its reason' { $fa.deltas.'r.lines' -eq 3 -and $fa.deltas.'page:docs/big.md' -eq 11 -and $fa.reason -eq 'alpha grows' -and @(Get-ChildItem (Join-Path $a 'structure-raises')).Count -eq 1 }
  Copy-Item -LiteralPath (Join-Path $base 'systems/renderer/src/r.cpp') -Destination (Join-Path $a 'systems/renderer/src/r.cpp') -Force
  [void](Invoke-Check $a @{ Raise = 'A' })
  $fa = Get-RaiseFile $a 'A'
  Test-That 'and drops an entry that is back at zero' { -not $fa.deltas.PSObject.Properties['r.lines'] -and $fa.deltas.'page:docs/big.md' -eq 11 }
  Copy-Item -LiteralPath (Join-Path $base 'docs/big.md') -Destination (Join-Path $a 'docs/big.md') -Force
  [void](Invoke-Check $a @{ Raise = 'A' })
  Test-That 'and the file with its last entry' { -not (Test-Path -LiteralPath (Join-Path $a 'structure-raises/A.json')) -and (Invoke-Check $a).Code -eq 0 }
  [void](Test-Reports 'a name that differs from a file''s only in case is refused, on every host' $b "the directory spells 'b.json' as 'B.json'" @{ Raise = 'b'; Reason = 'x' })

  Write-Host 'case: a hub that shrank'
  $s = New-Fixture
  Set-FixtureFile $s 'systems/renderer/src/r.cpp' (([IO.File]::ReadAllLines((Join-Path $s 'systems/renderer/src/r.cpp')) | Select-Object -First 10) -join "`n")
  [void](Test-Reports 'fails against its effective ceiling' $s 'r.lines: 10 lines, 3 under its ceiling of 13')
  $r = Invoke-Check $s @{ Raise = 'S' }
  Test-That 'a file of negative deltas only needs no reason, and the branch passes' { $r.Code -eq 0 -and (Get-RaiseFile $s 'S').deltas.'r.lines' -eq -3 -and (Invoke-Check $s).Code -eq 0 }

  Write-Host 'case: a raise file that is wrong'
  $w = New-Fixture
  New-Item -ItemType Directory -Force -Path (Join-Path $w 'structure-raises') | Out-Null
  Set-FixtureFile $w 'structure-raises/W.json' '{ "format": "engine.structure-raise.v1", "name": "W", "reason": "", "deltas": { "r.lines": 4 } }'
  [void](Test-Reports 'a positive delta with no reason fails' $w 'structure-raises/W.json: raises r.lines and gives no reason')
  Set-FixtureFile $w 'structure-raises/W.json' '{ "format": "engine.structure-raise.v1", "name": "W", "reason": "typo", "deltas": { "r.line": 4 } }'
  [void](Test-Reports 'a delta for no hub fails' $w "structure-raises/W.json: 'r.line' names no hub")
  Set-FixtureFile $w 'structure-raises/W.json' '{ "format": "engine.structure-raise.v1", "name": "W", "reason": "", "deltas": { "page:docs/ledger.md": -1 } }'
  [void](Test-Reports 'a delta for an unpinned page fails' $w "'page:docs/ledger.md' names no hub")

  Write-Host 'case: the fold'
  $r = Invoke-Check $m @{ Rebaseline = $true; Date = '2026-11-10'; By = 'coordinator' }
  $spec = Get-Ceilings $m
  Test-That 'needs no reason when the files cover every raise' { $r.Code -eq 0 -and (Get-Ceiling $m 'r.lines') -eq 18 -and (Get-Ceiling $m 'page:docs/big.md') -eq 330 }
  Test-That 'moves each file''s name, reason and deltas into rebaselines' {
    $rec = @($spec.rebaselines)[-1]
    @($rec.folded).Count -eq 2 -and $rec.folded[0].name -eq 'A' -and $rec.folded[0].reason -eq 'alpha grows' -and $rec.folded[1].deltas.'r.lines' -eq 3 -and
    @($rec.changes | Where-Object { $_.id -eq 'r.lines' -and $_.old -eq 13 -and $_.new -eq 18 }).Count -eq 1
  }
  Test-That 'deletes the files, and the tree passes' { @(Get-ChildItem (Join-Path $m 'structure-raises')).Count -eq 0 -and (Invoke-Check $m).Code -eq 0 }
  $o = New-Fixture; Add-Growth $o 2 10 'alpha'
  [void](Test-Reports 'refuses without a reason while a value is over its effective ceiling' $o 'r.lines: 13 -> 15' @{ Rebaseline = $true })
} finally {
  foreach ($root in $roots) {
    if ($KeepTemp) { Write-Host "kept $root" } else { Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue }
  }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "structure-check: $checks checks passed" -ForegroundColor Green
exit 0
