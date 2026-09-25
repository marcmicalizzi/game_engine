#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Banned-pattern lint for engine sources (docs/plan/11-performance-principles.md section 11.2).

.DESCRIPTION
  Scans engine source directories for node-based standard containers and ownership
  types that are replaced by core/containers, exception usage, and includes that are
  confined to part of the tree, and fails if any appear outside the allowed locations.
  A line may opt out with a trailing comment:

      // engine-lint: allow-std-container <reason>
      // engine-lint: allow-exceptions <reason>
      // engine-lint: allow-flecs <reason>
      // engine-lint: allow-temp-path <reason>
      // engine-lint: allow-vulkan <reason>

  A file may opt out entirely with the same marker anywhere in its first 5 lines.

  Allowed locations without a marker: tools/, tests/ directories, bench/ directories,
  third_party/, build/ — except for rules that name their own `AllowedPaths`, which
  are confined to those paths wherever they appear, and rules whose Scope is 'test',
  which apply *only* inside tests/ and bench/ directories and nowhere else. A rule with a
  `PathPattern` applies only to files whose repository-relative path matches it, and a rule
  with `CodeOnly` reads a line with its comments removed.
#>
[CmdletBinding()]
param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$engineRoots = @('core', 'foundation', 'domain', 'systems', 'apps', 'game')
# `tools/` and the shared `tests/` directory are outside the engine tree and are deliberately not
# held to the container, exception, or include rules — AGENTS.md says so. They are scanned anyway,
# for the rules whose Scope is 'test': those are not about what engine code may contain but about
# what a *test* may do to the machine it runs on, and a test under tools/ runs on the same machine.
$scanRoots = ($engineRoots + @('tools', 'tests')) |
  ForEach-Object { Join-Path $Root $_ } | Where-Object { Test-Path $_ }

# A rule with `AllowedPaths` is a *confinement* rule: the pattern is legal inside those
# directories — including their tests and benches — and illegal everywhere else it is scanned.
# `Scope = 'all'` says so, because the tests/ and bench/ exemption that makes sense for a
# container choice makes no sense for an include that decides which modules a library reaches.
# `Scope = 'test'` is the mirror image: the rule applies inside tests/ and bench/ directories and
# only there.
$rules = @(
  @{ Name = 'std-container'; Marker = 'allow-std-container'; Scope = 'engine'
     Pattern = 'std::(map|multimap|set|multiset|unordered_map|unordered_multimap|unordered_set|unordered_multiset|list|forward_list|deque|shared_ptr|weak_ptr|make_shared)\b'
     Message = 'banned node-based container or shared ownership; use core/containers or unique ownership' },
  @{ Name = 'exceptions'; Marker = 'allow-exceptions'; Scope = 'engine'
     Pattern = '^\s*(throw\b|try\s*\{|catch\s*\()|\bstd::(exception|runtime_error|logic_error)\b'
     Message = 'exceptions are not used across engine code; return a result' },
  # ADR-0028 seam 5. flecs is exposed directly and deliberately not wrapped, which only stays a
  # bounded decision if the set of modules that can see it is bounded: the cost of ever replacing
  # it is "rewrite the system bodies and queries", and that is only true while physics, nav, anim,
  # gfx, geometry, sim, store and everything in core/ and foundation/ have never heard of it.
  # Bridging code between an ECS-free module and the world lives in systems/.
  @{ Name = 'flecs-include'; Marker = 'allow-flecs'; Scope = 'all'
     Pattern = '^\s*#\s*include\s*[<"]flecs'
     AllowedPaths = @('domain/ecs', 'systems', 'game')
     Message = 'flecs.h belongs to domain/ecs, systems/ and game/ (ADR-0028 seam 5); lower modules stay ECS-free and bridging code lives in systems/' },
  # AGENTS.md "Test hygiene". A path a test computes from the system temp directory is a path the
  # *next copy of that test* computes identically: another worktree's build, a release suite beside
  # a debug one, a second agent, a developer running the suite while CI runs it. The two runs then
  # delete each other's fixtures, and the failure wears a convincing disguise (fails in a full run,
  # passes when re-run alone). Nineteen files got this wrong before it was noticed, which is why it
  # is a lint and not a review item. `engine::test::TempDir` names the directory instead, and
  # tests/support is the one place allowed to say `temp_directory_path`, because that is where
  # TempDir computes the root all of this lives under.
  @{ Name = 'test-temp-path'; Marker = 'allow-temp-path'; Scope = 'test'
     Pattern = '\btemp_directory_path\b|\btmpnam\b|\bGetTempPath[AW2]?\b|\bgetenv\s*\(\s*"(TEMP|TMP|TMPDIR)"'
     AllowedPaths = @('tests/support')
     # `Pending` is a deadline, not an exemption: a path listed here is known to be wrong and is
     # being converted by whoever owns it, so the lint reports it on every run and still exits 0.
     # Empty since 2026-09-18, when the last fixed temp names (engine-view's kept fixture copies)
     # went; keep it empty.
     Pending = @()
     Message = 'a test or bench names no path under the system temp directory; take scratch space from engine::test::TempDir (<test_temp_dir.h>), see AGENTS.md "Test hygiene"' },
  # docs/subsystems/gfx.md, "The RHI surface and the backend surface"; plan 10 §10.6, 2026-09-25.
  # A public header is what every module that includes it compiles against, so a Vulkan type in
  # one is a Vulkan dependency in all of them — and a second backend (D3D12 behind plan 08 §8.3's
  # trigger, or a console's API) cannot live beside a renderer whose headers say VkBuffer. Until
  # 2026-09-25 eleven of gfx's seventeen public headers and the renderer's did, and the leak grew
  # every week, which is why this is a lint and not a review item. The backend's own headers live
  # under the two named `backend/vulkan/` directories and are the only ones allowed to. Comments
  # are not read (`CodeOnly`): naming the Vulkan extension a capability comes from is
  # documentation of the backend, not a type the includer compiles against. The marker exists for
  # a stated exception; the exemptions are meant to be zero, and are.
  @{ Name = 'vulkan-in-public-header'; Marker = 'allow-vulkan'; Scope = 'all'; CodeOnly = $true
     PathPattern = '(^|/)include/'
     Pattern = '\bVk[A-Z]\w+|\bVK_[A-Z][A-Z0-9_]*|^\s*#\s*include\s*[<"](vulkan/|volk\.h|vk_mem_alloc\.h)'
     AllowedPaths = @('domain/gfx/include/domain/gfx/backend/vulkan',
                      'foundation/window/include/foundation/window/backend/vulkan')
     # The rule landed with 217 lines in fourteen headers on this list; the change that moved gfx,
     # the renderer and the window onto the engine's vocabulary emptied it the same day. Keep it
     # empty.
     Pending = @()
     Message = 'a public header names no Vulkan type, constant or header; say it in the engine''s vocabulary (domain/gfx/rhi.h) or put it in a backend/vulkan/ header, see docs/subsystems/gfx.md "The RHI surface and the backend surface"' },
  # The other half of the same decision: the backend header set, and the Vulkan, volk and VMA
  # headers behind it, are confined to the modules that own a device, a swapchain or a surface —
  # gfx itself and its tests, the window system's surface glue, and engine-view, which presents.
  # `systems/renderer` records into an image the caller names and needs none of it; were it to
  # include the backend, a second backend could not run it. The build enforces this as well (only
  # targets that link engine::gfx_vulkan see volk.h); the lint says why, and says it first.
  @{ Name = 'vulkan-backend-include'; Marker = 'allow-vulkan'; Scope = 'all'
     Pattern = '^\s*#\s*include\s*[<"](domain/gfx/backend/|foundation/window/backend/|vulkan/|volk\.h|vk_mem_alloc\.h)'
     AllowedPaths = @('domain/gfx', 'foundation/window', 'apps/engine_view')
     Message = 'the Vulkan backend set (domain/gfx/backend/vulkan/, foundation/window/backend/vulkan/, volk, VMA, vulkan/*) is included only by domain/gfx, foundation/window and apps/engine_view; everything else uses the engine''s RHI vocabulary, see docs/subsystems/gfx.md "The RHI surface and the backend surface"' }
)

$violations = New-Object System.Collections.Generic.List[string]
$pending = New-Object System.Collections.Generic.List[string]
$scanned = 0

# A path is inside one of a rule's allowed roots. Compared on the repository-relative path with
# forward slashes, so the answer does not depend on the platform's separator.
function Test-AllowedPath([string]$relative, [string[]]$allowed) {
  foreach ($prefix in $allowed) {
    if ($relative -eq $prefix -or $relative.StartsWith("$prefix/")) { return $true }
  }
  return $false
}

foreach ($dir in $scanRoots) {
  $files = Get-ChildItem -Path $dir -Recurse -Include *.h, *.hpp, *.cpp, *.inl -File |
    Where-Object { $_.FullName -notmatch '[\\/](third_party|build|_deps)[\\/]' }
  foreach ($file in $files) {
    # Reported with forward slashes whatever the platform, so a message is the same string on
    # Windows and on CI's Linux runners and a test can assert on it.
    $rel = ([IO.Path]::GetRelativePath($Root, $file.FullName)) -replace '\\', '/'
    $isTestOrBench = $file.FullName -match '[\\/](tests|bench)[\\/]'
    $inEngineTree = Test-AllowedPath $rel $engineRoots
    $scanned++
    # @() because Get-Content returns a bare string for a one-line file, and indexing a string
    # yields its characters: a one-line file's violation was read as the single character '#'.
    $lines = @(Get-Content -LiteralPath $file.FullName)
    $head = ($lines | Select-Object -First 5) -join "`n"
    foreach ($rule in $rules) {
      if ($rule.Scope -eq 'test') {
        if (-not $isTestOrBench) { continue }
      } else {
        # Everything that is not a 'test'-scoped rule is about engine code, and tools/ and the
        # shared tests/ directory are not engine code.
        if (-not $inEngineTree) { continue }
        if ($rule.Scope -ne 'all' -and $isTestOrBench) { continue }
      }
      if ($rule.PathPattern -and $rel -notmatch $rule.PathPattern) { continue }
      if ($rule.AllowedPaths -and (Test-AllowedPath $rel $rule.AllowedPaths)) { continue }
      if ($head -match "engine-lint:\s*$($rule.Marker)") { continue }
      for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        # A CodeOnly rule reads what the compiler reads: a `//` comment, a one-line `/* */` and a
        # line that continues a block comment (` * ...`) are dropped first. Good enough for the
        # tree's headers, which comment with `//`; a miss here is a missed report, never a false one.
        $code = $line
        if ($rule.CodeOnly) {
          $code = $code -replace '/\*.*?\*/', '' -replace '//.*$', ''
          if ($code -match '^\s*(\*|/\*)') { $code = '' }
        }
        if ($code -match $rule.Pattern -and $line -notmatch "engine-lint:\s*$($rule.Marker)") {
          $report = "{0}:{1}: [{2}] {3}`n    {4}" -f $rel, ($i + 1), $rule.Name, $rule.Message, $line.Trim()
          if ($rule.Pending -and (Test-AllowedPath $rel $rule.Pending)) {
            $pending.Add($report)
          } else {
            $violations.Add($report)
          }
        }
      }
    }
  }
}

if ($pending.Count -gt 0) {
  Write-Host "lint: $($pending.Count) known violation(s) on a rule's Pending list (not a failure)" -ForegroundColor Yellow
  $pending | ForEach-Object { Write-Host $_ }
}

if ($violations.Count -gt 0) {
  Write-Host "lint: $($violations.Count) violation(s) in $scanned file(s)" -ForegroundColor Red
  $violations | ForEach-Object { Write-Host $_ }
  exit 1
}

Write-Host "lint: OK ($scanned source files scanned)"
exit 0
