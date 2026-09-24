#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/docs-check.ps1. Runs under CTest as `tools.docs_check`.

.DESCRIPTION
  Builds a small documented tree in a temporary directory — two modules, an app, two ADRs, a
  plan page, an experiment write-up — checks that it passes, and then breaks it one way at a
  time: a module with no page, a page with no module, a link to nothing, an anchor that is not a
  heading, a status the template does not allow, an ADR nobody indexed. Each case asserts the
  message *and* that the run failed, because a check that reports a problem and exits 0 is worse
  than no check at all.

  Every case gets its own copy of the fixture, so a case cannot pass because of what another one
  left behind.

      pwsh tools/docs-check.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'

$check = Join-Path $PSScriptRoot 'docs-check.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$roots = New-Object System.Collections.Generic.List[string]

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) {
    Write-Host "  ok   $what"
  } else {
    Write-Host "  FAIL $what" -ForegroundColor Red
    $failures.Add($what)
  }
}

function Invoke-Check([string]$root) {
  # The report is written with Write-Host, which is the information stream, not output: *>&1
  # brings every stream back so the tests can read what a contributor would see.
  $out = (& $check -Root $root *>&1 | Out-String)
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

# The check failed, and it said this about it. The expected text is a literal, because the
# message is the product: an agent reading it has to know which file to open.
function Test-Reports([string]$what, [string]$root, [string]$expected) {
  $script:checks++
  $result = Invoke-Check $root
  if ($result.Code -eq 1 -and $result.Out.Contains($expected)) {
    Write-Host "  ok   $what"
  } else {
    Write-Host "  FAIL $what (exit $($result.Code), expected to see: $expected)" -ForegroundColor Red
    Write-Host ($result.Out -split "`n" | ForEach-Object { "       $_" }) -Separator "`n"
    $failures.Add($what)
  }
}

function Test-Passes([string]$what, [string]$root) {
  $script:checks++
  $result = Invoke-Check $root
  if ($result.Code -eq 0) {
    Write-Host "  ok   $what"
  } else {
    Write-Host "  FAIL $what (exit $($result.Code))" -ForegroundColor Red
    Write-Host ($result.Out -split "`n" | ForEach-Object { "       $_" }) -Separator "`n"
    $failures.Add($what)
  }
}

function Set-FixtureFile([string]$root, [string]$rel, [string]$text) {
  $path = Join-Path $root $rel
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
  $text = $text -replace "`r`n", "`n"
  if (-not $text.EndsWith("`n")) { $text += "`n" }
  [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

function Add-FixtureLine([string]$root, [string]$rel, [string]$text) {
  $path = Join-Path $root $rel
  $existing = (Get-Content -LiteralPath $path -Raw) -replace "`r`n", "`n"
  Set-FixtureFile $root $rel ($existing + $text)
}

# A tree that passes: two modules with pages and rows, an app named on the apps page and in its
# row, two indexed ADRs, a plan page in the plan's index, an experiment with its write-up.
function New-Fixture {
  $root = Join-Path ([IO.Path]::GetTempPath()) "engine-docs-check-$([guid]::NewGuid().ToString('N'))"
  $roots.Add($root)

  Set-FixtureFile $root 'CMakeLists.txt' "project(Fixture)`nadd_subdirectory(core)`nadd_subdirectory(apps)`n"
  Set-FixtureFile $root 'core/CMakeLists.txt' "add_subdirectory(base)`nadd_subdirectory(containers)`n"
  Set-FixtureFile $root 'core/base/CMakeLists.txt' "engine_module(NAME base LAYER core)`n"
  Set-FixtureFile $root 'core/containers/CMakeLists.txt' "engine_module(NAME containers LAYER core`n  DEPS base`n  SOURCES src/containers.cpp)`n"
  Set-FixtureFile $root 'apps/CMakeLists.txt' "add_subdirectory(engine_cli)`n"
  Set-FixtureFile $root 'apps/engine_cli/CMakeLists.txt' "engine_app(NAME engine_cli OUTPUT engine-cli SOURCES main.cpp DEPS base)`n"

  Set-FixtureFile $root 'docs/subsystems/README.md' @'
# Subsystem pages

| Module | Layer | Page |
|---|---|---|
| base | core | [base.md](base.md) |
| containers | core | [containers.md](containers.md) |
| engine_cli | apps | [apps.md](apps.md) |
'@
  Set-FixtureFile $root 'docs/subsystems/base.md' "# base (core)`n`n**Purpose.** The bottom of the tree.`n"
  Set-FixtureFile $root 'docs/subsystems/containers.md' "# containers (core)`n`n**Purpose.** The engine container set.`n"
  Set-FixtureFile $root 'docs/subsystems/apps.md' "# apps: engine-cli (apps)`n`n**Purpose.** ``engine-cli`` is the scriptable client.`n"

  Set-FixtureFile $root 'docs/adr/0000-template.md' @'
# ADR-NNNN: <short title>

- **Status:** Proposed | Accepted | Superseded by ADR-MMMM
- **Date:** YYYY-MM-DD
- **Plan references:** docs/plan/<file> §<section>

## Context

What forced a decision.
'@
  Set-FixtureFile $root 'docs/adr/0001-first-decision.md' @'
# ADR-0001: The first decision

- **Status:** Accepted
- **Date:** 2026-01-01

## Context

Something had to be decided.
'@
  Set-FixtureFile $root 'docs/adr/0002-second-decision.md' @'
# ADR-0002: The second decision

- **Status:** Accepted
- **Date:** 2026-01-02

## Context

So did something else.
'@
  Set-FixtureFile $root 'docs/adr/README.md' @'
# Architecture Decision Records

Numbered and immutable once accepted. Use [0000-template.md](0000-template.md).

| ADR | Title | Status |
|---|---|---|
| [0001](0001-first-decision.md) | The first decision | Accepted |
| [0002](0002-second-decision.md) | The second decision | Accepted |
'@

  Set-FixtureFile $root 'docs/plan/README.md' @'
# Plan

| Document | Covers |
|---|---|
| [01-shape](01-shape.md) | the shape of the thing |

Experiment write-ups live in [docs/experiments](../experiments/).
'@
  Set-FixtureFile $root 'docs/plan/01-shape.md' @'
# 01 — The shape

## 1.1 A section

The first decision is [ADR-0001](../adr/0001-first-decision.md#context), the shape is
[§1.1](#11-a-section), and `base` has [a page](../subsystems/base.md#base-core).

| ID | Question | Method | Decides |
|---|---|---|---|
| E1 | Does it work? | Measure it | Everything. **Done 2026-01-02** ([results](../experiments/e1-thing.md)) |
'@
  Set-FixtureFile $root 'docs/experiments/e1-thing.md' "# E1 — Does it work?`n`nIt does.`n"

  Set-FixtureFile $root 'AGENTS.md' "# Working in this repository`n`nRead the plan first.`n"
  Set-FixtureFile $root 'docs/experiments/README.md' "# Experiments`n`nOne write-up per experiment.`n"
  Set-FixtureFile $root 'README.md' @'
# Fixture engine

Status lives in the plan's ledger. Layers: `core/` holds the bottom of the tree and `apps/` the
executables; `engine-cli` is the scriptable client.

- [docs/plan/README.md](docs/plan/README.md)
- [docs/adr/README.md](docs/adr/README.md)
- [docs/subsystems/README.md](docs/subsystems/README.md)
- [docs/experiments/README.md](docs/experiments/README.md)
- [AGENTS.md](AGENTS.md)
'@
  return $root
}

try {
  Write-Host 'case: a tree that follows the rule'
  $root = New-Fixture
  Test-Passes 'the fixture passes as built' $root
  Test-That 'the report counts what it read' { (Invoke-Check $root).Out -match 'docs-check: OK \(\d+ markdown files' }

  Write-Host 'case: a module with no page'
  $root = New-Fixture
  Set-FixtureFile $root 'core/math/CMakeLists.txt' "engine_module(NAME math LAYER core DEPS base)`n"
  Test-Reports 'names the manifest that declared it' $root `
    "core/math/CMakeLists.txt:1: [subsystems] module 'math' has no docs/subsystems/math.md"

  Write-Host 'case: a page that is in no index'
  $root = New-Fixture
  Set-FixtureFile $root 'core/math/CMakeLists.txt' "engine_module(NAME math LAYER core DEPS base)`n"
  Set-FixtureFile $root 'docs/subsystems/math.md' "# math (core)`n`n**Purpose.** Vectors.`n"
  Test-Reports 'an unlisted page is a failure' $root 'docs/subsystems/math.md is not listed in the table'

  Write-Host 'case: a row whose layer is not where the module lives'
  $root = New-Fixture
  Set-FixtureFile $root 'foundation/io/CMakeLists.txt' "engine_module(NAME io LAYER foundation DEPS base)`n"
  Set-FixtureFile $root 'docs/subsystems/io.md' "# io (foundation)`n`n**Purpose.** Files.`n"
  Add-FixtureLine $root 'docs/subsystems/README.md' "| io | core | [io.md](io.md) |`n"
  Test-Reports 'the layer has to match the directory' $root `
    "module 'io' lives in foundation/ but the table says the core layer"

  Write-Host 'case: a stale page'
  $root = New-Fixture
  Set-FixtureFile $root 'docs/subsystems/ghost.md' "# ghost (core)`n`n**Purpose.** A module that was deleted.`n"
  Add-FixtureLine $root 'docs/subsystems/README.md' "| ghost | core | [ghost.md](ghost.md) |`n"
  Test-Reports 'a page for a module that does not exist' $root `
    "docs/subsystems/ghost.md:1: [subsystems] no module 'ghost' is declared"

  Write-Host 'case: an app nobody documented'
  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_view/CMakeLists.txt' "engine_app(NAME engine_view OUTPUT engine-view SOURCES main.cpp DEPS base)`n"
  Test-Reports 'the apps page must name it' $root "app 'engine_view' is not mentioned in docs/subsystems/apps.md"
  Set-FixtureFile $root 'docs/subsystems/apps.md' "# apps: engine-cli, engine-view (apps)`n`n**Purpose.** ``engine-cli`` and ``engine-view``.`n"
  Test-Reports 'and so must its row' $root "app 'engine_view' is missing from the apps row of the table"

  Write-Host 'case: a link to nothing'
  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' "`nSee [the missing decision](../adr/0009-missing.md).`n"
  Test-Reports 'the link and the line that holds it' $root "docs/plan/01-shape.md:12: [link] '../adr/0009-missing.md' does not exist"

  Write-Host 'case: an anchor that is not a heading'
  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' "`nSee [the wrong section](../adr/0001-first-decision.md#decision).`n"
  Test-Reports 'names the file the anchor was looked for in' $root `
    "[link] '#decision' is not a heading in docs/adr/0001-first-decision.md"

  Write-Host 'case: anchors follow GitHub s slug rules'
  $root = New-Fixture
  Add-FixtureLine $root 'docs/subsystems/base.md' "`n## Notes`n`nfirst`n`n## Notes`n`nsecond`n`n## 2.5 Repository layout: the rules`n"
  Add-FixtureLine $root 'docs/plan/01-shape.md' @'

[first](../subsystems/base.md#notes), [second](../subsystems/base.md#notes-1),
[punctuation dropped](../subsystems/base.md#25-repository-layout-the-rules).
'@
  Test-Passes 'duplicate headings, punctuation, and numbers resolve' $root
  Add-FixtureLine $root 'docs/plan/01-shape.md' "`n[a third one](../subsystems/base.md#notes-2).`n"
  Test-Reports 'and a third duplicate does not exist' $root "'#notes-2' is not a heading"

  Write-Host 'case: the escape comment'
  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' @'

<!-- docs-check: ignore-link the manifest only exists after a configure -->
The build writes [a manifest](../../build/msvc-debug/shaders/manifest.json).
'@
  Test-Passes 'a link with a reason on the line before is allowed' $root
  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' @'

<!-- docs-check: ignore-link -->
The build writes [a manifest](../../build/msvc-debug/shaders/manifest.json).
'@
  Test-Reports 'an escape without a reason is not' $root 'ignore-link escape needs a reason'

  Write-Host 'case: links inside code fences are examples, not links'
  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' "`n" + '```markdown' + "`n[a template link](nowhere.md)`n" + '```' + "`n"
  Test-Passes 'a fenced example is not checked' $root

  Write-Host 'case: ADR status'
  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0002-second-decision.md' "# ADR-0002: The second decision`n`n- **Status:** Probably`n- **Date:** 2026-01-02`n"
  Test-Reports 'a status the template does not allow' $root "'Probably' is not one of: Proposed | Accepted | Superseded by ADR-MMMM"

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0002-second-decision.md' "# ADR-0002: The second decision`n`n- **Status:** Superseded by ADR-0009`n- **Date:** 2026-01-02`n"
  Test-Reports 'superseded by an ADR that does not exist' $root "names ADR-0009, which does not exist"

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0002-second-decision.md' "# ADR-0002: The second decision`n`n- **Date:** 2026-01-02`n`n## Context`n`nNo status at all.`n"
  Test-Reports 'no status line' $root "no '**Status:**' line"

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0002-second-decision.md' "# ADR-0002: The second decision`n`n- **Status:** Superseded by ADR-0001`n- **Date:** 2026-01-02`n"
  Set-FixtureFile $root 'docs/adr/README.md' @'
# Architecture Decision Records

Use [0000-template.md](0000-template.md).

| ADR | Title | Status |
|---|---|---|
| [0001](0001-first-decision.md) | The first decision | Accepted |
| [0002](0002-second-decision.md) | The second decision | Superseded by ADR-0001 |
'@
  Test-Passes 'superseded by an ADR that does exist' $root

  Write-Host 'case: the ADR index'
  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0003-third-decision.md' "# ADR-0003: The third decision`n`n- **Status:** Proposed`n- **Date:** 2026-01-03`n"
  Test-Reports 'an ADR nobody indexed' $root '0003-third-decision.md is not listed in the index'

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0004-fourth-decision.md' "# ADR-0004: The fourth decision`n`n- **Status:** Proposed`n- **Date:** 2026-01-04`n"
  Add-FixtureLine $root 'docs/adr/README.md' "| [0004](0004-fourth-decision.md) | The fourth decision | Proposed |`n"
  Test-Reports 'a gap in the numbers' $root 'no ADR-0003, but ADR-0004 exists'

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/README.md' @'
# Architecture Decision Records

Use [0000-template.md](0000-template.md).

| ADR | Title | Status |
|---|---|---|
| [0001](0001-first-decision.md) | The first decision | Accepted |
| [0002](0002-second-decision.md) | A different title entirely | Proposed |
'@
  Test-Reports 'a title the index invented' $root "the title differs from the ADR's own"
  Test-Reports 'a status the index is behind on' $root "the status differs from the ADR's own"

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/README.md' @'
# Architecture Decision Records

Use [0000-template.md](0000-template.md).

| ADR | Title | Status |
|---|---|---|
| [0001](0001-first-decision.md) | The first decision | Accepted |
| [0002](0002-second-decision.md) | The second decision (revisited after E1) | Accepted |
'@
  Test-Passes 'the index may add a note after the title' $root

  Write-Host 'case: references to ADRs and experiments'
  $root = New-Fixture
  Add-FixtureLine $root 'docs/subsystems/base.md' "`nThis follows ADR-0042.`n"
  Test-Reports 'an ADR number nobody wrote' $root 'ADR-0042 is referenced but docs/adr/0042-*.md does not exist'

  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' "| E2 | Does that work? | Measure it | Everything. **Done 2026-01-03** |`n"
  Test-Reports 'a finished experiment with no write-up' $root 'reports a result but links no write-up'

  $root = New-Fixture
  Add-FixtureLine $root 'docs/plan/01-shape.md' "| E3 | Will it work? | It is measured by a harness | Not yet run |`n"
  Test-Passes 'prose about measuring is not a result' $root

  Write-Host 'case: the plan index'
  $root = New-Fixture
  Set-FixtureFile $root 'docs/plan/02-scope.md' "# 02 — Scope`n`nIn scope: this.`n"
  Test-Reports 'a plan page the plan does not list' $root "docs/plan/02-scope.md is not listed in the plan's document table"

  Write-Host 'case: the root README'
  $root = New-Fixture
  Set-FixtureFile $root 'README.md' @'
# Fixture engine

Layers: `core/` and `apps/`. See [docs/plan/README.md](docs/plan/README.md), [docs/adr/README.md](docs/adr/README.md),
[docs/subsystems/README.md](docs/subsystems/README.md), [docs/experiments/README.md](docs/experiments/README.md) and [AGENTS.md](AGENTS.md).
'@
  Test-Reports 'a root README that names no executable is reported' $root `
    "README.md:1: [readme] the root README does not name the executable 'engine_cli'"
  $root = New-Fixture
  Set-FixtureFile $root 'README.md' @'
# Fixture engine

Layers: `core/` and `apps/`; `engine-cli` is the client. See [docs/plan/README.md](docs/plan/README.md),
[docs/subsystems/README.md](docs/subsystems/README.md), [docs/experiments/README.md](docs/experiments/README.md) and [AGENTS.md](AGENTS.md).
'@
  Test-Reports 'a root README that drops an index link is reported' $root `
    'README.md:1: [readme] the root README does not link docs/adr/README.md'
  $root = New-Fixture
  Remove-Item -LiteralPath (Join-Path $root 'README.md')
  Test-Reports 'a repository with no root README is reported' $root `
    'README.md:1: [readme] the repository has no README.md at its root'

  Write-Host 'case: one run, every problem'
  $root = New-Fixture
  Set-FixtureFile $root 'core/math/CMakeLists.txt' "engine_module(NAME math LAYER core DEPS base)`n"
  Set-FixtureFile $root 'docs/subsystems/ghost.md' "# ghost (core)`n`n**Purpose.** Gone.`n"
  Add-FixtureLine $root 'docs/plan/01-shape.md' "`nSee [nothing](../adr/0009-missing.md).`n"
  $result = Invoke-Check $root
  Test-That 'three problems, three lines, one run' {
    ($result.Code -eq 1) -and
    ($result.Out -match 'docs-check: 3 problem\(s\)') -and
    $result.Out.Contains("module 'math' has no") -and
    $result.Out.Contains("no module 'ghost' is declared") -and
    $result.Out.Contains("'../adr/0009-missing.md' does not exist")
  }
  Test-That 'the problems are sorted by file and line' {
    $lines = @($result.Out -split "`n" | Where-Object { $_ -match '^\s+\S+:\d+: \[' } | ForEach-Object { $_.Trim() })
    ($lines.Count -eq 3) -and ($lines[0] -like 'core/math/*') -and ($lines[1] -like 'docs/plan/*') -and ($lines[2] -like 'docs/subsystems/*')
  }
} finally {
  foreach ($root in $roots) {
    if ($KeepTemp) {
      Write-Host "kept $root"
    } else {
      Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue
    }
  }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "docs-check: $checks checks passed" -ForegroundColor Green
exit 0
