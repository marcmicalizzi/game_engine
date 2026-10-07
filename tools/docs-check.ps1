#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Checks the "Documentation moves with the code" rule (AGENTS.md) mechanically.

.DESCRIPTION
  AGENTS.md promises that a module has a page, that an ADR is numbered and indexed, and that
  the documents point at each other correctly. This script is that promise, checked:

    subsystems  every engine_module() has docs/subsystems/<module>.md, listed in that
                directory's README with the layer it lives in; every engine_app() is named
                in docs/subsystems/apps.md and in its README row; no page without a module.
    adr         every docs/adr/NNNN-*.md has a **Status:** the template allows, numbers are
                unique and contiguous, and docs/adr/README.md lists each one with its title
                and its status.
    link        every relative Markdown link resolves to a file, and an anchor resolves to a
                heading in it under GitHub's slug rules.
    adr-ref     every ADR-NNNN mentioned in the documents is an ADR that exists.
    experiment  an E<n> row of the plan's experiment table that says Done or Measured links
                its write-up under docs/experiments/.
    plan        docs/plan/README.md's document table lists every docs/plan page.
    roadmap     docs/roadmap.md's item ids are unique, every id a "Depends on" cell names is a
                row, a done row names what closed it (an ADR, a commit, a dated status note), a
                blocked row says by what, every row has a Where link, and State, Effort and
                Reasoning use the page's own vocabulary.
    readme      the root README.md exists, names every layer directory and every engine_app(),
                and links AGENTS.md, the plan, ADR, subsystems and experiments indexes, and the
                roadmap when there is one.

  Every failure is reported as file:line, and one run reports all of them: a check that stops
  at the first problem costs a round trip per problem.

  A link that genuinely cannot resolve (a path that only exists after a build, a file in
  another repository) opts out with a reason on the line before it:

      <!-- docs-check: ignore-link the manifest only exists after a configure -->
      [shaders/manifest.json](../../build/msvc-debug/shaders/manifest.json)

  Runs under CTest as `docs_check`; tools/docs-check.Tests.ps1 is its own test.

.EXAMPLE
  pwsh tools/docs-check.ps1
  pwsh tools/docs-check.ps1 -Root /path/to/a/fixture/tree
#>
[CmdletBinding()]
param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path -LiteralPath $Root).Path
$problems = New-Object System.Collections.Generic.List[object]

# Directories that are not ours to check: build output, vendored code, and the worktrees and
# caches agents leave behind. They are skipped by name while walking, never entered, so a tree
# with a build directory in it costs nothing to check.
$excludedDirs = @('build', 'out', '_deps', 'third_party', '.git', '.claude', '.cache', 'node_modules', 'ddc')

# Directories that exist only on a machine that downloaded them: their documents are somebody
# else's and their links are not this repository's to fix (tools/fetch-samples.ps1).
$excludedPaths = @('content/samples')

function Get-Rel([string]$path) {
  return ([IO.Path]::GetRelativePath($Root, $path) -replace '\\', '/')
}

function Add-Problem([string]$path, [int]$line, [string]$rule, [string]$message) {
  $problems.Add([pscustomobject]@{ File = (Get-Rel $path); Line = $line; Rule = $rule; Message = $message })
}

# Files under a directory, skipping the excluded ones, in sorted order: the same tree always
# produces the same report, in the same order (docs/plan/11-performance-principles.md).
function Get-TreeFiles([string]$dir, [string]$pattern) {
  $found = New-Object System.Collections.Generic.List[string]
  if (-not (Test-Path -LiteralPath $dir)) { return @() }
  $pending = New-Object System.Collections.Generic.Stack[string]
  $pending.Push((Resolve-Path -LiteralPath $dir).Path)
  while ($pending.Count -gt 0) {
    $current = $pending.Pop()
    foreach ($sub in [IO.Directory]::EnumerateDirectories($current)) {
      if ($excludedDirs -contains [IO.Path]::GetFileName($sub)) { continue }
      if ($excludedPaths -contains (Get-Rel $sub)) { continue }
      $pending.Push($sub)
    }
    foreach ($file in [IO.Directory]::EnumerateFiles($current, $pattern)) { $found.Add($file) }
  }
  return @($found | Sort-Object)
}

function Get-FileLines([string]$path) {
  $text = [IO.File]::ReadAllText($path)
  return (($text -replace "`r`n", "`n") -split "`n")
}

function Get-LineOf([string]$text, [int]$index) {
  # 1-based line number of a regex match inside a whole-file string.
  return ([regex]::Matches($text.Substring(0, $index), "`n").Count + 1)
}

# ---- markdown ----------------------------------------------------------------------------------

# GitHub's heading slug: drop HTML and link syntax, lower case, drop punctuation, spaces become
# hyphens. Underscores and hyphens survive, so `engine_module` stays `engine_module`. Repeated
# headings get -1, -2, ... in document order, which the caller counts.
function ConvertTo-Anchor([string]$heading) {
  $text = $heading
  $text = [regex]::Replace($text, '<[^>]+>', '')
  $text = [regex]::Replace($text, '!?\[([^\]]*)\]\([^)]*\)', '$1')
  $text = $text -replace '[`*~]', ''
  $text = $text.ToLowerInvariant()
  $text = [regex]::Replace($text, '[^\p{L}\p{Nd}\p{M} _-]', '')
  return ($text -replace ' ', '-')
}

$markdownCache = @{}

# lines, the anchors their headings define, and which lines are inside a fenced code block —
# a fence holds examples, and an example's links and headings are not the document's.
function Get-MarkdownInfo([string]$path) {
  $key = ([IO.Path]::GetFullPath($path)).ToLowerInvariant()
  if ($markdownCache.ContainsKey($key)) { return $markdownCache[$key] }

  $lines = Get-FileLines $path
  $anchors = New-Object System.Collections.Generic.HashSet[string]
  $fenced = New-Object System.Collections.Generic.List[bool]
  $seen = @{}
  $inFence = $false
  $fenceMarker = ''

  foreach ($line in $lines) {
    $isFenceLine = $false
    $first = if ($line.Length -gt 0) { $line[0] } else { [char]0 }
    if (($first -eq '`' -or $first -eq '~' -or $first -eq ' ') -and $line -match '^\s{0,3}(```+|~~~+)') {
      $marker = $Matches[1].Substring(0, 3)
      if (-not $inFence) {
        $inFence = $true
        $fenceMarker = $marker
        $isFenceLine = $true
      } elseif ($marker -eq $fenceMarker) {
        $inFence = $false
        $isFenceLine = $true
      }
    }
    $fenced.Add($inFence -or $isFenceLine)

    if ($first -eq '#' -and -not $inFence -and -not $isFenceLine -and $line -match '^(#{1,6})\s+(.*?)\s*#*\s*$') {
      $slug = ConvertTo-Anchor $Matches[2]
      if ($slug) {
        if ($seen.ContainsKey($slug)) {
          $n = $seen[$slug]
          $seen[$slug] = $n + 1
          [void]$anchors.Add("$slug-$n")
        } else {
          $seen[$slug] = 1
          [void]$anchors.Add($slug)
        }
      }
    }
  }

  $info = [pscustomobject]@{ Path = $path; Lines = $lines; Anchors = $anchors; Fenced = $fenced }
  $markdownCache[$key] = $info
  return $info
}

function Get-CMakeManifests([string]$root) {
  $found = New-Object System.Collections.Generic.List[string]
  foreach ($layer in @('core', 'foundation', 'domain', 'systems', 'apps', 'game')) {
    foreach ($file in (Get-TreeFiles (Join-Path $root $layer) 'CMakeLists.txt')) { $found.Add($file) }
  }
  return @($found)
}

# ---- what the tree declares --------------------------------------------------------------------

# One row per engine_module() and engine_app(), with where it was declared and the layer
# directory it lives in — the directory is the layer a docs row has to agree with, since that is
# what a reader sees.
$modules = @{}
$apps = @{}

foreach ($manifest in Get-CMakeManifests $Root) {
  $text = ([IO.File]::ReadAllText($manifest) -replace "`r`n", "`n")
  $rel = Get-Rel $manifest
  $layerDir = ($rel -split '/')[0]

  foreach ($m in [regex]::Matches($text, 'engine_module\(\s*NAME\s+(\w+)\s+LAYER\s+(\w+)')) {
    $name = $m.Groups[1].Value
    if ($modules.ContainsKey($name)) { continue }   # one module, declared twice behind an if()
    $modules[$name] = [pscustomobject]@{
      Name     = $name
      Layer    = $m.Groups[2].Value
      LayerDir = $layerDir
      File     = $manifest
      Line     = Get-LineOf $text $m.Index
    }
  }
  foreach ($m in [regex]::Matches($text, 'engine_app\(\s*NAME\s+(\w+)(?:\s+OUTPUT\s+([\w-]+))?')) {
    $name = $m.Groups[1].Value
    if ($apps.ContainsKey($name)) { continue }
    $apps[$name] = [pscustomobject]@{
      Name   = $name
      Output = $m.Groups[2].Value
      File   = $manifest
      Line   = Get-LineOf $text $m.Index
    }
  }
}

# ---- subsystem pages ----------------------------------------------------------------------------

$subsystemsDir = Join-Path $Root 'docs/subsystems'
$subsystemsReadme = Join-Path $subsystemsDir 'README.md'
$readmeRows = @{}          # page file name -> row
$appsRowText = ''
$readmeTableLine = 1

if (Test-Path -LiteralPath $subsystemsReadme) {
  $info = Get-MarkdownInfo $subsystemsReadme
  for ($i = 0; $i -lt $info.Lines.Count; $i++) {
    if ($info.Fenced[$i]) { continue }
    $line = $info.Lines[$i]
    if ($line -notmatch '^\s*\|') { continue }
    $cells = @(($line.Trim() -replace '^\|', '' -replace '\|$', '') -split '\|')
    if ($cells.Count -lt 3) { continue }
    if ($cells[2] -notmatch '\[([^\]]+)\]\(([^)#]+)(?:#[^)]*)?\)') { continue }
    $page = $Matches[2].Trim()
    $readmeTableLine = $i + 1
    $readmeRows[$page] = [pscustomobject]@{
      Page    = $page
      Modules = $cells[0].Trim()
      Layer   = $cells[1].Trim()
      Line    = $i + 1
    }
    if ($page -eq 'apps.md') { $appsRowText = $cells[0].Trim() }
  }
} else {
  Add-Problem $subsystemsReadme 1 'subsystems' 'the subsystem index is missing'
}

foreach ($name in ($modules.Keys | Sort-Object)) {
  $module = $modules[$name]
  $page = Join-Path $subsystemsDir "$name.md"
  if (-not (Test-Path -LiteralPath $page)) {
    Add-Problem $module.File $module.Line 'subsystems' `
      "module '$name' has no docs/subsystems/$name.md. AGENTS.md: documentation moves with the code."
    continue
  }
  if (-not $readmeRows.ContainsKey("$name.md")) {
    Add-Problem $subsystemsReadme $readmeTableLine 'subsystems' `
      "docs/subsystems/$name.md is not listed in the table (module '$name', declared in $(Get-Rel $module.File))"
    continue
  }
  $row = $readmeRows["$name.md"]
  if ($row.Layer -ne $module.LayerDir) {
    Add-Problem $subsystemsReadme $row.Line 'subsystems' `
      "module '$name' lives in $($module.LayerDir)/ but the table says the $($row.Layer) layer"
  }
}

# The apps are one page; it and its row name each of them, since an app nobody names is an app
# nobody finds.
$appsPage = Join-Path $subsystemsDir 'apps.md'
foreach ($name in ($apps.Keys | Sort-Object)) {
  $app = $apps[$name]
  $spellings = @($name, ($name -replace '_', '-'))
  if ($app.Output) { $spellings += $app.Output }
  $pattern = ($spellings | ForEach-Object { [regex]::Escape($_) }) -join '|'

  if (-not (Test-Path -LiteralPath $appsPage)) {
    Add-Problem $app.File $app.Line 'subsystems' "app '$name' has no docs/subsystems/apps.md to be documented on"
    continue
  }
  if (([IO.File]::ReadAllText($appsPage)) -notmatch $pattern) {
    Add-Problem $app.File $app.Line 'subsystems' `
      "app '$name' is not mentioned in docs/subsystems/apps.md"
  }
  if ($appsRowText -and $appsRowText -notmatch $pattern) {
    Add-Problem $subsystemsReadme $readmeRows['apps.md'].Line 'subsystems' `
      "app '$name' is missing from the apps row of the table"
  }
}

# The root README is the first page a stranger reads and the one page no module owns, so it
# rotted: it said "Phase 0, nothing renders yet" for nine days of Phase 1 while every other
# page moved with the code. It cannot be checked for truth, but it can be checked for shape:
# it exists, it names every layer directory and every executable, and it links the four
# indexes that do carry the current state. A README that fails this is a README that lies by
# omission, which is the failure it had.
$rootReadme = Join-Path $Root 'README.md'
if (-not (Test-Path -LiteralPath $rootReadme)) {
  Add-Problem $rootReadme 1 'readme' 'the repository has no README.md at its root'
} else {
  $rootReadmeText = [IO.File]::ReadAllText($rootReadme)
  foreach ($layerDir in @('core', 'foundation', 'domain', 'systems', 'apps', 'game')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $layerDir))) { continue }
    if ($rootReadmeText -notmatch ('`' + [regex]::Escape($layerDir) + '/`')) {
      Add-Problem $rootReadme 1 'readme' "the root README does not name the layer directory '$layerDir/'"
    }
  }
  foreach ($name in ($apps.Keys | Sort-Object)) {
    $app = $apps[$name]
    $spellings = @($name, ($name -replace '_', '-'))
    if ($app.Output) { $spellings += $app.Output }
    $pattern = ($spellings | ForEach-Object { [regex]::Escape($_) }) -join '|'
    if ($rootReadmeText -notmatch $pattern) {
      Add-Problem $rootReadme 1 'readme' "the root README does not name the executable '$name'"
    }
  }
  # The roadmap is where the live state is kept, so the page that points a stranger at the state
  # has to point at it too, once there is one.
  $indexes = @('AGENTS.md', 'docs/plan/README.md', 'docs/adr/README.md', 'docs/subsystems/README.md', 'docs/experiments/README.md')
  if (Test-Path -LiteralPath (Join-Path $Root 'docs/roadmap.md')) { $indexes += 'docs/roadmap.md' }
  foreach ($index in $indexes) {
    if ($rootReadmeText -notmatch ('\(' + [regex]::Escape($index) + '(#[^)]*)?\)')) {
      Add-Problem $rootReadme 1 'readme' "the root README does not link $index"
    }
  }
}

# A page for something that no longer exists is worse than no page: it is documentation that
# lies. README.md is the index and apps.md covers every engine_app().
if (Test-Path -LiteralPath $subsystemsDir) {
  foreach ($page in (Get-ChildItem -LiteralPath $subsystemsDir -Filter *.md -File | Sort-Object Name)) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($page.Name)
    if ($page.Name -eq 'README.md' -or $page.Name -eq 'apps.md') { continue }
    if (-not $modules.ContainsKey($stem)) {
      Add-Problem $page.FullName 1 'subsystems' `
        "no module '$stem' is declared; a page for a module that does not exist is stale"
    }
  }
}

foreach ($page in ($readmeRows.Keys | Sort-Object)) {
  $target = Join-Path $subsystemsDir $page
  if (-not (Test-Path -LiteralPath $target)) {
    Add-Problem $subsystemsReadme $readmeRows[$page].Line 'subsystems' "the table lists $page, which does not exist"
  }
}

# ---- ADRs ---------------------------------------------------------------------------------------

$adrDir = Join-Path $Root 'docs/adr'
$adrTemplate = Join-Path $adrDir '0000-template.md'
$adrs = @{}
$allowedStatuses = @()

if (Test-Path -LiteralPath $adrTemplate) {
  # The allowed statuses are the template's, so the two cannot drift apart.
  $templateStatus = (Get-FileLines $adrTemplate | Where-Object { $_ -match '\*\*Status:\*\*\s*(.+?)\s*$' } | Select-Object -First 1)
  if ($templateStatus -match '\*\*Status:\*\*\s*(.+?)\s*$') {
    $allowedStatuses = @($Matches[1] -split '\s*\|\s*' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
  }
}
if (-not $allowedStatuses) { $allowedStatuses = @('Proposed', 'Accepted', 'Superseded by ADR-MMMM') }

# 'Superseded by ADR-MMMM' is a shape, not a word: MMMM is whichever ADR superseded this one.
function Test-AdrStatus([string]$status, [hashtable]$known, [ref]$reason) {
  foreach ($allowed in $allowedStatuses) {
    if ($allowed -notmatch 'MMMM') {
      if ($status -eq $allowed) { return $true }
      continue
    }
    $pattern = '^' + ([regex]::Escape($allowed) -replace 'MMMM', '(\d{4})') + '$'
    if ($status -match $pattern) {
      $target = [int]$Matches[1]
      if (-not $known.ContainsKey($target)) {
        $reason.Value = "'$status' names ADR-$($Matches[1]), which does not exist"
        return $false
      }
      return $true
    }
  }
  $reason.Value = "'$status' is not one of: $($allowedStatuses -join ' | ')"
  return $false
}

if (Test-Path -LiteralPath $adrDir) {
  foreach ($file in (Get-ChildItem -LiteralPath $adrDir -Filter '*.md' -File | Sort-Object Name)) {
    if ($file.Name -notmatch '^(\d{4})-(.+)\.md$') { continue }
    $number = [int]$Matches[1]
    if ($number -eq 0) { continue }   # the template
    $lines = Get-FileLines $file.FullName
    $title = ''
    $titleNumber = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match '^#\s+ADR-(\d{4}):\s*(.+?)\s*$') {
        $titleNumber = [int]$Matches[1]
        $title = $Matches[2]
        break
      }
      if ($lines[$i] -match '^#\s+(.+?)\s*$') { $title = $Matches[1]; break }
    }
    $status = ''
    $statusLine = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match '\*\*Status:\*\*\s*(.+?)\s*$') {
        $status = $Matches[1]
        $statusLine = $i + 1
        break
      }
    }
    $adrs[$number] = [pscustomobject]@{
      Number      = $number
      File        = $file.FullName
      Name        = $file.Name
      Title       = $title
      TitleNumber = $titleNumber
      Status      = $status
      StatusLine  = $statusLine
    }
  }
}

foreach ($number in ($adrs.Keys | Sort-Object)) {
  $adr = $adrs[$number]
  if (-not $adr.Status) {
    Add-Problem $adr.File 1 'adr' "no '**Status:**' line; copy docs/adr/0000-template.md"
  } else {
    $reason = ''
    if (-not (Test-AdrStatus $adr.Status $adrs ([ref]$reason))) {
      Add-Problem $adr.File $adr.StatusLine 'adr' $reason
    }
  }
  if ($adr.TitleNumber -ge 0 -and $adr.TitleNumber -ne $number) {
    Add-Problem $adr.File 1 'adr' "the title says ADR-$('{0:0000}' -f $adr.TitleNumber) but the file is $($adr.Name)"
  }
}

# Contiguous from 1: a gap is a lost decision or a number taken twice, and both are worth a
# failure rather than a shrug.
$numbers = @($adrs.Keys | Sort-Object)
if ($numbers.Count -gt 0) {
  for ($n = 1; $n -le $numbers[-1]; $n++) {
    if (-not $adrs.ContainsKey($n)) {
      Add-Problem $adrDir 1 'adr' ("no ADR-{0:0000}, but ADR-{1:0000} exists; the numbers are contiguous" -f $n, $numbers[-1])
    }
  }
}

$adrReadme = Join-Path $adrDir 'README.md'
$adrRows = @{}
$adrTableLine = 1
if (Test-Path -LiteralPath $adrReadme) {
  $info = Get-MarkdownInfo $adrReadme
  for ($i = 0; $i -lt $info.Lines.Count; $i++) {
    if ($info.Fenced[$i]) { continue }
    $line = $info.Lines[$i]
    if ($line -notmatch '^\s*\|') { continue }
    $cells = @(($line.Trim() -replace '^\|', '' -replace '\|$', '') -split '\|')
    if ($cells.Count -lt 3) { continue }
    if ($cells[0] -notmatch '\[(\d{4})\]\(([^)]+)\)') { continue }
    $adrTableLine = $i + 1
    $adrRows[[int]$Matches[1]] = [pscustomobject]@{
      Number = [int]$Matches[1]
      Target = $Matches[2].Trim()
      Title  = $cells[1].Trim()
      Status = $cells[2].Trim()
      Line   = $i + 1
    }
  }
} elseif ($adrs.Count -gt 0) {
  Add-Problem $adrReadme 1 'adr' 'the ADR index is missing'
}

foreach ($number in ($adrs.Keys | Sort-Object)) {
  $adr = $adrs[$number]
  if (-not $adrRows.ContainsKey($number)) {
    Add-Problem $adrReadme $adrTableLine 'adr' "$($adr.Name) is not listed in the index"
    continue
  }
  $row = $adrRows[$number]
  if ($row.Target -ne $adr.Name) {
    Add-Problem $adrReadme $row.Line 'adr' "the row for ADR-$('{0:0000}' -f $number) links $($row.Target), but the file is $($adr.Name)"
  }
  # The index carries the ADR's own title, and may add a note after it ("(E2 result)") — an
  # accepted ADR is immutable, so the index is where a later fact about it can be recorded.
  if ($adr.Title -and -not $row.Title.StartsWith($adr.Title)) {
    Add-Problem $adrReadme $row.Line 'adr' "the title differs from the ADR's own: index says '$($row.Title)', $($adr.Name) says '$($adr.Title)'"
  }
  if ($adr.Status -and $row.Status -ne $adr.Status) {
    Add-Problem $adrReadme $row.Line 'adr' "the status differs from the ADR's own: index says '$($row.Status)', $($adr.Name) says '$($adr.Status)'"
  }
}
foreach ($number in ($adrRows.Keys | Sort-Object)) {
  if (-not $adrs.ContainsKey($number)) {
    Add-Problem $adrReadme $adrRows[$number].Line 'adr' "the index lists ADR-$('{0:0000}' -f $number), which does not exist"
  }
}

# ---- links, ADR references, experiment write-ups -------------------------------------------------

$linkRegex = [regex]'!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*([^)\s]+)(?:\s+"[^"]*")?\s*\)'
$ignoreRegex = [regex]'<!--\s*docs-check:\s*ignore-link\b\s*(.*?)\s*-->'

$markdownFiles = Get-TreeFiles $Root '*.md'
$linkCount = 0

foreach ($file in $markdownFiles) {
  $info = Get-MarkdownInfo $file
  $dir = Split-Path -Parent $file
  $rel = Get-Rel $file
  $isTemplate = ($rel -eq 'docs/adr/0000-template.md')
  $isPlan = $rel.StartsWith('docs/plan/')

  # Most lines are prose with nothing to check in them. A substring test costs a fraction of the
  # regex it stands in front of, and this loop runs over every line of every document.
  for ($i = 0; $i -lt $info.Lines.Count; $i++) {
    $rawLine = $info.Lines[$i]

    if ($rawLine.Contains('docs-check:') -and $rawLine -match $ignoreRegex -and -not $Matches[1]) {
      Add-Problem $file ($i + 1) 'link' 'a docs-check: ignore-link escape needs a reason after it'
    }
    if ($info.Fenced[$i]) { continue }

    # ADR-NNNN in prose: the number has to be an ADR that exists. The template's ADR-NNNN and
    # ADR-MMMM are placeholders, not references.
    if (-not $isTemplate -and $rawLine.Contains('ADR-')) {
      foreach ($m in [regex]::Matches($rawLine, 'ADR-(\d{4})')) {
        $number = [int]$m.Groups[1].Value
        if ($number -eq 0) { continue }
        if (-not $adrs.ContainsKey($number)) {
          Add-Problem $file ($i + 1) 'adr-ref' "ADR-$($m.Groups[1].Value) is referenced but docs/adr/$($m.Groups[1].Value)-*.md does not exist"
        }
      }
    }

    # An experiment row of the plan that reports a result owes the reader the write-up. The
    # marker is the table's own: a bold **Done <date>** or **Measured**, or a cell that is only
    # that word — case-sensitive, so prose about something being measured is prose.
    if ($isPlan -and $rawLine.StartsWith('| E') -and
        ($rawLine -cmatch '\*\*(Done|Measured)\b' -or $rawLine -cmatch '\|\s*(Done|Measured)\b')) {
      if ([regex]::Matches($rawLine, '\]\([^)]*experiments/[^)#]+').Count -eq 0) {
        Add-Problem $file ($i + 1) 'experiment' `
          'this experiment reports a result but links no write-up under docs/experiments/'
      }
    }

    if (-not $rawLine.Contains('](')) { continue }
    if ($i -gt 0 -and $info.Lines[$i - 1].Contains('docs-check:') -and $info.Lines[$i - 1] -match $ignoreRegex) { continue }

    # Inline code is text, not a link: `[a](b)` in a sentence about syntax is not a reference.
    $line = if ($rawLine.Contains('`')) { $rawLine -replace '`[^`]*`', '' } else { $rawLine }
    foreach ($m in $linkRegex.Matches($line)) {
      $target = $m.Groups[1].Value
      if ($target -match '^(https?|mailto|ftp|tel|data):' -or $target.StartsWith('<')) { continue }
      $linkCount++

      $path = $target
      $anchor = ''
      $hash = $target.IndexOf('#')
      if ($hash -ge 0) {
        $path = $target.Substring(0, $hash)
        $anchor = $target.Substring($hash + 1)
      }
      $path = [uri]::UnescapeDataString($path)

      $targetFile = $file
      if ($path) {
        if ($path.StartsWith('/')) {
          $targetFile = [IO.Path]::GetFullPath((Join-Path $Root $path.TrimStart('/')))
        } else {
          $targetFile = [IO.Path]::GetFullPath((Join-Path $dir $path))
        }
        if (-not (Test-Path -LiteralPath $targetFile)) {
          Add-Problem $file ($i + 1) 'link' "'$target' does not exist"
          continue
        }
      }

      if ($anchor -and $targetFile.EndsWith('.md', [StringComparison]::OrdinalIgnoreCase)) {
        $targetInfo = Get-MarkdownInfo $targetFile
        if (-not $targetInfo.Anchors.Contains($anchor.ToLowerInvariant())) {
          $where = if ($path) { (Get-Rel $targetFile) } else { 'this file' }
          Add-Problem $file ($i + 1) 'link' "'#$anchor' is not a heading in $where"
        }
      }
    }
  }
}

# ---- the roadmap ----------------------------------------------------------------------------------

# docs/roadmap.md is the live board of every development item. Other rows name an item by its id,
# so ids are unique and a "Depends on" cell names only rows that exist; a done row says what closed
# it, so "done" is a claim a reader can follow to an ADR, a commit or a dated status note; a blocked
# row says by what; and every row points somewhere, since the board holds titles, not descriptions.
# Its links are checked above with every other document's. State, Effort and Reasoning are checked
# against the page's own vocabulary, so a typo cannot hide a row from whoever filters the board.
# An item table is one whose header row's first cell is '#'; other tables on the page are prose.
function Get-RoadmapCell($cells, [hashtable]$columns, [string]$name) {
  if (-not $columns.ContainsKey($name)) { return '' }
  $c = $columns[$name]
  if ($c -lt $cells.Count) { return $cells[$c] }
  return ''
}

$roadmapPath = Join-Path $Root 'docs/roadmap.md'
$roadmapItemCount = 0
if (Test-Path -LiteralPath $roadmapPath) {
  $info = Get-MarkdownInfo $roadmapPath
  $roadmapIds = @{}
  $roadmapRows = New-Object System.Collections.Generic.List[object]
  $columns = $null
  $skipTable = $false
  $requiredColumns = @('#', 'Item', 'State', 'Depends on', 'Effort', 'Reasoning', 'Where')

  for ($i = 0; $i -lt $info.Lines.Count; $i++) {
    $line = $info.Lines[$i]
    if ($info.Fenced[$i] -or $line -notmatch '^\s*\|') { $columns = $null; continue }   # a table ends at its first other line
    $cells = @((($line.Trim() -replace '\\\|', '/') -replace '^\|', '' -replace '\|$', '') -split '\|' | ForEach-Object { $_.Trim() })

    if ($null -eq $columns) {
      if ($cells[0] -ne '#') { $columns = @{}; $skipTable = $true; continue }
      $columns = @{}
      for ($c = 0; $c -lt $cells.Count; $c++) { $columns[$cells[$c]] = $c }
      $skipTable = $false
      foreach ($required in $requiredColumns) {
        if (-not $columns.ContainsKey($required)) {
          Add-Problem $roadmapPath ($i + 1) 'roadmap' "this item table has no '$required' column; the columns are: $($requiredColumns -join ', ')"
          $skipTable = $true
        }
      }
      continue
    }
    if ($skipTable -or $cells[0] -match '^:?-+:?$') { continue }

    $id = $cells[0]
    if ($id -notmatch '^[A-Z]\d+$') {
      Add-Problem $roadmapPath ($i + 1) 'roadmap' "'$id' is not an item id: an area letter and a number, like R12"
      continue
    }
    if ($roadmapIds.ContainsKey($id)) {
      Add-Problem $roadmapPath ($i + 1) 'roadmap' "$id is used twice (first on line $($roadmapIds[$id])); an id is never reused or renumbered"
      continue
    }
    $roadmapIds[$id] = $i + 1
    $roadmapRows.Add([pscustomobject]@{
      Id        = $id
      Line      = $i + 1
      State     = Get-RoadmapCell $cells $columns 'State'
      Depends   = Get-RoadmapCell $cells $columns 'Depends on'
      Effort    = Get-RoadmapCell $cells $columns 'Effort'
      Reasoning = Get-RoadmapCell $cells $columns 'Reasoning'
      Where     = Get-RoadmapCell $cells $columns 'Where'
    })
  }

  $roadmapItemCount = $roadmapRows.Count
  if ($roadmapRows.Count -eq 0) {
    Add-Problem $roadmapPath 1 'roadmap' "no item rows: an item table's header row starts with '#'"
  }

  foreach ($row in $roadmapRows) {
    $state = $row.State
    if ($state -match '^(done|in progress|open|blocked|deferred)\b') {
      $word = $Matches[1]
      # A commit is seven or more hex digits with at least one digit among them, so a word that
      # happens to be spelled in a-f is not taken for one.
      if ($word -eq 'done' -and $state -notmatch 'ADR-\d{4}|\b(?=[0-9a-f]*\d)[0-9a-f]{7,40}\b|\d{4}-\d{2}-\d{2}') {
        Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id) is done but does not say what closed it: name the ADR, the commit or the dated status note"
      }
      if ($word -eq 'blocked' -and $state -notmatch '^blocked\W+\w') {
        Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id) is blocked but does not say by what"
      }
    } else {
      Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id)'s state '$state' is not one of: done, in progress, open, blocked, deferred"
    }

    foreach ($token in @(($row.Depends -replace '[—–]', ' ') -split '[,;\s]+' | Where-Object { $_ -and $_ -ne '-' })) {
      if ($token -notmatch '^[A-Z]\d+$') {
        Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id) depends on '$token', which is not an item id"
      } elseif (-not $roadmapIds.ContainsKey($token)) {
        Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id) depends on $token, which is no row's id"
      }
    }

    if ($row.Effort -notmatch '^(S|M|L)$') {
      Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id)'s effort '$($row.Effort)' is not one of: S, M, L"
    }
    if ($row.Reasoning -notmatch '^(routine|design|research)$') {
      Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id)'s reasoning '$($row.Reasoning)' is not one of: routine, design, research"
    }
    if (-not $row.Where.Contains('](')) {
      Add-Problem $roadmapPath $row.Line 'roadmap' "$($row.Id) has no Where link: the plan section, ADR, experiment or page to read"
    }
  }
}

# ---- the plan's own index -------------------------------------------------------------------------

$planDir = Join-Path $Root 'docs/plan'
$planReadme = Join-Path $planDir 'README.md'
if ((Test-Path -LiteralPath $planDir) -and (Test-Path -LiteralPath $planReadme)) {
  $planText = [IO.File]::ReadAllText($planReadme)
  $listed = New-Object System.Collections.Generic.HashSet[string]
  foreach ($m in [regex]::Matches($planText, '\]\(([^)#]+\.md)')) {
    [void]$listed.Add(([IO.Path]::GetFileName($m.Groups[1].Value)))
  }
  foreach ($page in (Get-TreeFiles $planDir '*.md')) {
    $name = [IO.Path]::GetFileName($page)
    if ($name -eq 'README.md') { continue }
    if (-not $listed.Contains($name)) {
      Add-Problem $planReadme 1 'plan' "docs/plan/$name is not listed in the plan's document table"
    }
  }
}

# ---- report ----------------------------------------------------------------------------------------

if ($problems.Count -gt 0) {
  Write-Host "docs-check: $($problems.Count) problem(s)" -ForegroundColor Red
  foreach ($p in ($problems | Sort-Object File, Line, Rule, Message)) {
    Write-Host ("  {0}:{1}: [{2}] {3}" -f $p.File, $p.Line, $p.Rule, $p.Message)
  }
  Write-Host ''
  Write-Host 'The rule is "Documentation moves with the code" in AGENTS.md: a module has a page, an'
  Write-Host 'ADR is numbered and indexed, every link resolves, the root README names what exists, and'
  Write-Host 'the roadmap''s rows name ids that exist and say what closed them.'
  Write-Host 'Fix the documents, not this check.'
  exit 1
}

Write-Host ("docs-check: OK ({0} markdown files, {1} links, {2} modules, {3} apps, {4} ADRs, {5} roadmap items)" -f `
  $markdownFiles.Count, $linkCount, $modules.Count, $apps.Count, $adrs.Count, $roadmapItemCount)
exit 0
