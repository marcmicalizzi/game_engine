#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/new-capability.ps1 (ADR-0027). Runs under CTest as `tools.new_capability`.

.DESCRIPTION
  Scaffolds into a throwaway tree — a few CMakeLists.txt files and a docs/subsystems/README.md,
  enough for the script to resolve dependencies and find its insertion points — and checks what
  came out: the files, the lines added to the files it does not own, the capability switch, and
  the refusals. No compiler is involved; that the generated sources build is proven by the
  generated module's own doctest run, which is the point of generating a passing test.

      pwsh tools/new-capability.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'

$scaffold = Join-Path $PSScriptRoot 'new-capability.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

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

function Test-Throws([string]$what, [string]$messageLike, [scriptblock]$action) {
  $script:checks++
  try {
    & $action | Out-Null
    Write-Host "  FAIL $what (nothing was thrown)" -ForegroundColor Red
    $failures.Add($what)
  } catch {
    if ($_.Exception.Message -like $messageLike) {
      Write-Host "  ok   $what"
    } else {
      Write-Host "  FAIL $what (message was: $($_.Exception.Message))" -ForegroundColor Red
      $failures.Add($what)
    }
  }
}

# clang-format, wherever this machine keeps it: on PATH (which `tools/dev.ps1 format` assumes),
# inside Visual Studio's bundled LLVM, or in a standalone LLVM install. Returns $null when there
# is none, and the caller then reports the formatting checks as skipped rather than failing — a
# hosted Linux runner has clang but not necessarily clang-format, and a test that fails for a
# missing optional tool teaches people to ignore it.
function Find-ClangFormat {
  $onPath = Get-Command clang-format -ErrorAction SilentlyContinue
  if ($onPath) { return $onPath.Source }
  $candidates = New-Object System.Collections.Generic.List[string]
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $vswhere) {
    $vs = & $vswhere -latest -property installationPath 2>$null
    if ($vs) { $candidates.Add((Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-format.exe')) }
  }
  if ($env:ProgramFiles) { $candidates.Add((Join-Path $env:ProgramFiles 'LLVM\bin\clang-format.exe')) }
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return $null
}

# Every generated C++ file, run through clang-format with the repository's own style: the output
# has to come back byte for byte, or a freshly scaffolded capability greets its new owner with a
# diff nobody wrote. The interesting parameter is the capability's *name*, because every comment
# and every include in the templates is interpolated with it.
function Test-FormatClean([string]$clangFormat, [string]$root, [string]$moduleDir, [string]$what) {
  $script:checks++
  $files = @(Get-ChildItem -Path (Join-Path $root $moduleDir) -Recurse -Include *.h, *.cpp -File)
  if ($files.Count -eq 0) {
    Write-Host "  FAIL $what (no generated C++ files found)" -ForegroundColor Red
    $failures.Add($what)
    return
  }
  $dirty = New-Object System.Collections.Generic.List[string]
  foreach ($file in $files) {
    $before = ([System.IO.File]::ReadAllText($file.FullName) -replace "`r`n", "`n")
    $after = ((& $clangFormat --style=file $file.FullName | Out-String) -replace "`r`n", "`n")
    if ($before.TrimEnd("`n") -ne $after.TrimEnd("`n")) {
      $dirty.Add([IO.Path]::GetRelativePath($root, $file.FullName))
    }
  }
  if ($dirty.Count -eq 0) {
    Write-Host "  ok   $what ($($files.Count) files)"
  } else {
    Write-Host "  FAIL ${what}: clang-format rewrites $($dirty -join ', ')" -ForegroundColor Red
    $failures.Add($what)
  }
}

function New-FakeTree {
  $root = Join-Path ([IO.Path]::GetTempPath()) "engine-new-capability-$([guid]::NewGuid().ToString('N'))"
  $files = @{
    'CMakeLists.txt'                = "project(Fake)`ninclude(EngineModule)`nadd_subdirectory(core)`nadd_subdirectory(domain)`n# add_subdirectory(systems)`nadd_subdirectory(apps)`nengine_finalize_modules()`n"
    'core/CMakeLists.txt'           = "add_subdirectory(base)`nadd_subdirectory(containers)`n"
    'core/base/CMakeLists.txt'      = "engine_module(NAME base LAYER core)`n"
    'core/containers/CMakeLists.txt' = "engine_module(NAME containers LAYER core DEPS base)`n"
    'domain/CMakeLists.txt'         = "add_subdirectory(protocol)`nadd_subdirectory(store)`n"
    'domain/protocol/CMakeLists.txt' = "engine_module(NAME protocol LAYER domain DEPS base)`n"
    # An optional capability to depend on, so the capability-graph case has something real to
    # find. Anything that ticks depends on one of these in the actual tree.
    'domain/store/CMakeLists.txt'   = "engine_module(NAME store LAYER domain OPTIONAL DEPS base)`n"
    'apps/CMakeLists.txt'           = "engine_app(NAME engine_cli OUTPUT engine-cli SOURCES main.cpp DEPS base)`n"
    'docs/subsystems/README.md'     = @"
# Subsystem pages

| Module | Layer | Page |
|---|---|---|
| base | core | [base.md](base.md) |
| containers | core | [containers.md](containers.md) |
| protocol | domain | [protocol.md](protocol.md) |
| store | domain | [store.md](store.md) |
| engine_cli | apps | [apps.md](apps.md) |
"@
  }
  foreach ($rel in $files.Keys) {
    $path = Join-Path $root $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllText($path, ($files[$rel] -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
  }
  return $root
}

function Get-Text([string]$root, [string]$rel) {
  $path = Join-Path $root $rel
  if (-not (Test-Path -LiteralPath $path)) { return '' }
  return (Get-Content -LiteralPath $path -Raw) -replace "`r`n", "`n"
}

$root = New-FakeTree
try {
  Write-Host "scaffolding into $root"

  # ---- a plain systems capability --------------------------------------------------------------
  Write-Host 'case: -Name cloth -Layer systems -Deps "base containers"'
  & $scaffold -Name cloth -Layer systems -Deps 'base containers' -Root $root | Out-Null

  foreach ($rel in @(
      'systems/cloth/CMakeLists.txt',
      'systems/cloth/include/systems/cloth/cloth.h',
      'systems/cloth/src/cloth.cpp',
      'systems/cloth/tests/cloth_tests.cpp',
      'systems/cloth/tests/size_table.cpp',
      'docs/subsystems/cloth.md')) {
    Test-That "generated $rel" { Test-Path -LiteralPath (Join-Path $root $rel) }
  }
  Test-That 'no schema without -WithSchema' { -not (Test-Path -LiteralPath (Join-Path $root 'systems/cloth/schemas/cloth.schema')) }
  Test-That 'no bench without -WithBench' { -not (Test-Path -LiteralPath (Join-Path $root 'systems/cloth/bench')) }

  $cmake = Get-Text $root 'systems/cloth/CMakeLists.txt'
  Test-That 'the module is OPTIONAL' { $cmake -match 'engine_module\(NAME cloth LAYER systems OPTIONAL' }
  Test-That 'the switch is named in the manifest' { $cmake -match 'ENGINE_WITH_CLOTH' }
  Test-That 'the declared deps are the requested ones' { $cmake -match 'DEPS base containers' }
  Test-That 'tests are registered' { $cmake -match 'engine_module_tests\(NAME cloth' }

  $header = Get-Text $root 'systems/cloth/include/systems/cloth/cloth.h'
  Test-That 'the system skeleton is there' { $header -match 'struct ClothSystem' }
  Test-That 'begin_tick/tick/end_tick are declared' {
    ($header -match 'void begin_tick') -and ($header -match 'void tick') -and ($header -match 'void end_tick')
  }
  Test-That 'the LOD policy is there' { $header -match 'lod_tier' }
  Test-That 'the determinism declaration is there' { $header -match 'k_determinism' }
  Test-That 'the registration checklist is there' { $header -match '\[ \] scheduler entry' }
  Test-That 'no protocol surface without -WithProtocol' { $header -notmatch 'register_methods' }

  Test-That 'the size table pins the system' {
    (Get-Text $root 'systems/cloth/tests/size_table.cpp') -match 'ENGINE_EXPECT_SIZE\(16, 8, cloth::ClothSystem\)'
  }
  Test-That 'the generated test includes doctest' {
    (Get-Text $root 'systems/cloth/tests/cloth_tests.cpp') -match 'doctest/doctest.h'
  }

  Test-That 'the layer CMakeLists was created and lists the module' {
    (Get-Text $root 'systems/CMakeLists.txt') -match '(?m)^add_subdirectory\(cloth\)$'
  }
  Test-That 'the root CMakeLists adds the systems layer, uncommented' {
    (Get-Text $root 'CMakeLists.txt') -match '(?m)^add_subdirectory\(systems\)$'
  }

  $readme = Get-Text $root 'docs/subsystems/README.md'
  Test-That 'the docs README has the row' { $readme -match '(?m)^\| cloth \| systems \| \[cloth\.md\]\(cloth\.md\) \|$' }
  Test-That 'the row is after domain and before apps' {
    $lines = $readme -split "`n"
    $domain = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match '^\| protocol \|' })
    $cloth = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match '^\| cloth \|' })
    $apps = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match '^\| engine_cli \|' })
    ($domain -lt $cloth) -and ($cloth -lt $apps)
  }
  Test-That 'the docs page carries the capability contract' {
    (Get-Text $root 'docs/subsystems/cloth.md') -match 'Capability contract \(ADR-0027\)'
  }
  # A generated page that only has a place for what the code does gets filled in with what the
  # code does; the why has to have a place of its own or it never gets written (AGENTS.md).
  Test-That 'the docs page asks for the why, not only the what' {
    $page = Get-Text $root 'docs/subsystems/cloth.md'
    ($page -match '(?m)^\*\*Why this shape\.\*\* TODO\(cloth\):') -and ($page -match 'Write the why, not only the what')
  }

  # ---- every switch on ---------------------------------------------------------------------------
  Write-Host 'case: -Name scent_field -WithSchema -WithBench -WithProtocol'
  & $scaffold -Name scent_field -Layer systems -Deps 'containers' -WithSchema -WithBench -WithProtocol -Root $root | Out-Null

  $cmake2 = Get-Text $root 'systems/scent_field/CMakeLists.txt'
  Test-That 'the schema library is tied to the capability switch' {
    $cmake2 -match 'engine_schema_library\(NAME scent_field_schemas SCHEMAS schemas/scent_field\.schema CAPABILITY scent_field\)'
  }
  Test-That 'the schema module is a dependency' { $cmake2 -match 'DEPS .*scent_field_schemas' }
  Test-That 'base is added when the caller forgets it' { $cmake2 -match 'DEPS base ' }
  Test-That 'the bench is registered' { $cmake2 -match 'engine_module_bench\(NAME scent_field' }
  Test-That 'the schema stub was generated' { Test-Path -LiteralPath (Join-Path $root 'systems/scent_field/schemas/scent_field.schema') }
  Test-That 'the schema stub declares the namespace' {
    (Get-Text $root 'systems/scent_field/schemas/scent_field.schema') -match '(?m)^namespace engine\.scent_field$'
  }
  Test-That 'the bench stub was generated' { Test-Path -LiteralPath (Join-Path $root 'systems/scent_field/bench/scent_field_bench.cpp') }
  $header2 = Get-Text $root 'systems/scent_field/include/systems/scent_field/scent_field.h'
  Test-That 'snake_case becomes PascalCase' { $header2 -match 'struct ScentFieldSystem' }
  Test-That 'the protocol registration is declared' { $header2 -match 'void register_methods\(protocol::Dispatcher& dispatcher\);' }
  Test-That 'the protocol registration is defined' {
    (Get-Text $root 'systems/scent_field/src/scent_field.cpp') -match 'void register_methods\(protocol::Dispatcher&\)'
  }
  Test-That 'the generated header includes its schema types' { $header2 -match '#include <schemas/scent_field\.h>' }
  Test-That 'a capability with no optional dependency declares no edge' {
    $cmake2 -notmatch 'engine_capability_requires'
  }

  # ---- the capability graph ----------------------------------------------------------------------
  # A capability that depends on another capability is the case ADR-0027's "revisit when"
  # anticipated and `systems/animation` hit first: without the declared edge, a configure with
  # ENGINE_WITH_<required>=OFF is a fatal error rather than one fewer capability.
  Write-Host 'case: -Name crowd -Deps "containers store" (store is OPTIONAL)'
  & $scaffold -Name crowd -Layer systems -Deps 'containers store' -Root $root | Out-Null
  $cmake3 = Get-Text $root 'systems/crowd/CMakeLists.txt'
  Test-That 'the edge to the required capability is declared' {
    $cmake3 -match '(?m)^engine_capability_requires\(crowd store\)$'
  }
  Test-That 'the edge comes before the module that needs it' {
    $cmake3.IndexOf('engine_capability_requires') -lt $cmake3.IndexOf('engine_module(')
  }
  Test-That 'the docs page records the requirement' {
    (Get-Text $root 'docs/subsystems/crowd.md') -match 'engine_capability_requires\(crowd store\)'
  }

  # ---- the scaffold is clang-format clean ---------------------------------------------------------
  # `tools/dev.ps1 format` on a freshly scaffolded capability must be a no-op. It was not: the
  # templated comment lines overrun the column limit for a name of the wrong length and clang-format
  # reflows them, and the include block's grouping depends on the capability's layer. Both are now
  # computed rather than written out, and this is what says so.
  Write-Host 'case: clang-format leaves the scaffold alone'
  $clangFormat = Find-ClangFormat
  if ($clangFormat) {
    Copy-Item (Join-Path $PSScriptRoot '..' '.clang-format') (Join-Path $root '.clang-format') -Force
    Test-FormatClean $clangFormat $root 'systems/cloth' 'a short name is format-clean'
    Test-FormatClean $clangFormat $root 'systems/scent_field' 'every switch on is format-clean'
    # The case the fix is actually about: a long name pushes every interpolated comment line and
    # every qualified name further right, and a template that only fits for `cloth` breaks here.
    & $scaffold -Name deformable_volume_field -Layer systems -Deps 'containers' -WithSchema -WithBench -WithProtocol -Root $root | Out-Null
    Test-FormatClean $clangFormat $root 'systems/deformable_volume_field' 'a long name is format-clean'
    # And a capability in another layer, where the include block sorts differently.
    & $scaffold -Name wind -Layer core -Deps 'containers' -WithBench -Root $root | Out-Null
    Test-FormatClean $clangFormat $root 'core/wind' 'a core-layer capability is format-clean'
    Test-That 'no generated comment line is over the column limit' {
      $long = Get-ChildItem -Path $root -Recurse -Include *.h, *.cpp -File |
        ForEach-Object { Get-Content -LiteralPath $_.FullName } |
        Where-Object { $_.Length -gt 100 }
      $long.Count -eq 0
    }
  } else {
    Write-Host '  skip clang-format was not found; the formatting checks did not run' -ForegroundColor Yellow
  }

  # ---- refusals -----------------------------------------------------------------------------------
  Write-Host 'case: refusals'
  Test-Throws 'refuses an existing module directory' '*already exists*' { & $scaffold -Name cloth -Root $root }
  Test-Throws 'refuses a name that is not snake_case' '*invalid capability name*' { & $scaffold -Name CamelCase -Root $root }
  Test-Throws 'refuses a name with a hyphen' '*invalid capability name*' { & $scaffold -Name 'two-words' -Root $root }
  Test-Throws 'refuses an unknown dependency' '*unknown dependency*' { & $scaffold -Name fluids -Deps 'no_such_module' -Root $root }
  Test-Throws 'refuses a dependency from a higher layer' '*above core*' { & $scaffold -Name fluids -Layer core -Deps 'protocol' -Root $root }
  Test-Throws 'refuses -WithProtocol below the domain layer' '*needs domain/protocol*' {
    & $scaffold -Name fluids -Layer foundation -WithProtocol -Root $root
  }
  Test-That 'a refused run left nothing behind' { -not (Test-Path -LiteralPath (Join-Path $root 'systems/fluids')) }
} finally {
  if ($KeepTemp) {
    Write-Host "kept $root"
  } else {
    Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue
  }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "new-capability: $checks checks passed" -ForegroundColor Green
exit 0
