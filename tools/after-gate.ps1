#!/usr/bin/env pwsh
<#
.SYNOPSIS
  After a green gate (roadmap T217; docs/ci/gate.md): main fast-forwarded to
  the gated tip, the release binaries copied out, installed into the local folder, the game author's
  SDK exported, the structure raises folded. The push is a switch it never takes by itself.

.DESCRIPTION
      pwsh tools/after-gate.ps1 -Gate <gate worktree> -Main <primary checkout> -Verdict <logs>/<batch>-gate.verdict.json
                                -CopyTo <dir> [-Install <dir>] [-Sdk <dir>] [-Trailer <line>] [-Push]

  It refuses (exit 2) unless the verdict file tools/gate.ps1 wrote says green and names the commit
  the gate worktree is at now, and unless -Main is on main and that commit is ahead of it: the
  binaries it installs are the ones the gate built from that very commit, which is what makes the
  fast-forward the merge (what-to-run.md, the persistent gate worktree).

  In order, each reported on one line:
    1. main fast-forwarded to the gated tip (`git merge --ff-only`; never a merge commit, never a reset);
    2. `<Gate>/build/<Preset>/bin` copied whole to -CopyTo: the next batch's "main" side of a pair;
    3. installed into -Install (default `<local folder>/bin-interactive`, the local folder being
       $env:ENGINE_LOCAL_ROOT or the folder beside the primary checkout, tools/lib/LocalRoot.psm1):
       every `engine-*` executable and its symbols, the game modules, the shell. A file in use by a
       running session is reported as NOT INSTALLED and left as it is: never forced, never a process
       stopped. Each installed file is compared with its source by hash;
    4. the SDK exported from the gate worktree (`tools/export-game-sdk.ps1`) into -Sdk (default
       `<local folder>/sdk/<short sha>`);
    5. the structure raises folded on main (`tools/structure-check.ps1 -Rebaseline`, AGENTS.md
       "Ceilings for structure") and committed, when there are any;
    6. pushed only with -Push.
  Exit 0 when every step did what it says, 1 when one did not (each named), 2 when it refused.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Gate,
  [Parameter(Mandatory)][string]$Main,
  [Parameter(Mandatory)][string]$Verdict,
  [Parameter(Mandatory)][string]$CopyTo,
  [string]$Install = '',
  [string]$Sdk = '',
  [string]$Preset = 'msvc-release',
  [string]$Trailer = '',
  [switch]$Push
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Gate.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib/LocalRoot.psm1') -Force

function Invoke-Git([string]$dir) { $o = & git -C $dir @args 2>&1; [pscustomobject]@{ Code = $LASTEXITCODE; Out = (@($o) -join "`n").Trim() } }

$Gate = (Resolve-Path -LiteralPath $Gate).Path
$Main = (Resolve-Path -LiteralPath $Main).Path
if (-not (Test-Path -LiteralPath $Verdict)) { "after-gate: no verdict file at $Verdict"; exit 2 }
$v = Get-Content -LiteralPath $Verdict -Raw | ConvertFrom-Json
$tip = (Invoke-Git $Gate rev-parse HEAD).Out
if (-not $v.green) { "after-gate: the gate $($v.batch) was not green ($(@($v.red) -join '; ')); nothing done"; exit 2 }
if ($v.tip -ne $tip) { "after-gate: the verdict is for $($v.tip) and the gate worktree is at $tip; nothing done"; exit 2 }
$branch = (Invoke-Git $Main rev-parse --abbrev-ref HEAD).Out
if ($branch -ne 'main') { "after-gate: $Main is on '$branch', not main; nothing done"; exit 2 }
$already = (Invoke-Git $Main merge-base --is-ancestor $tip HEAD).Code -eq 0   # a second run, after the fold
if (-not $already -and (Invoke-Git $Main merge-base --is-ancestor HEAD $tip).Code -ne 0) { "after-gate: main is not an ancestor of $tip; a fast-forward cannot reach it; nothing done"; exit 2 }
$logs = Split-Path -Parent (Resolve-Path -LiteralPath $Verdict).Path
$short = $tip.Substring(0, 8)
$problems = New-Object System.Collections.Generic.List[string]
"after $($v.batch): gate tip $short; untracked or changed in main: $(@((Invoke-Git $Main status --porcelain).Out -split "`n" | Where-Object { $_ })).Count)"

# 1. The fast-forward.
if ($already) { "MAIN already holds ${short}:$((Invoke-Git $Main log --oneline -1).Out)" }
else {
  $ff = Invoke-Git $Main merge --ff-only $tip
  if ($ff.Code -ne 0 -or (Invoke-Git $Main rev-parse HEAD).Out -ne $tip) { "MAIN not fast-forwarded: $($ff.Out)"; exit 1 }
  "MAIN $((Invoke-Git $Main log --oneline -1).Out)"
}

# 2. The copy out of the tree.
$bin = [IO.Path]::GetFullPath((Join-Path $Gate "build/$Preset/bin"))
if (-not (Test-Path -LiteralPath $bin)) { $problems.Add("no $Preset binaries at $bin"); "COPY skipped: no $bin" }
else {
  New-Item -ItemType Directory -Force -Path $CopyTo | Out-Null
  Copy-Item -Path (Join-Path $bin '*') -Destination $CopyTo -Recurse -Force
  "COPY $((Get-ChildItem -LiteralPath $CopyTo -File -Recurse | Measure-Object).Count) files to $CopyTo"
}

# 3. The install: a file in use is reported and left, never forced.
$local = Get-EngineLocalRoot -From $Main
if (-not $Install) { $Install = if ($local) { Join-Path $local 'bin-interactive' } else { '' } }
if (-not $Install) { $problems.Add('no install folder: give -Install or set ENGINE_LOCAL_ROOT'); 'INSTALL skipped: no folder' }
elseif (Test-Path -LiteralPath $bin) {
  New-Item -ItemType Directory -Force -Path $Install, (Join-Path $Install 'game') | Out-Null
  $files = @(Get-ChildItem -LiteralPath $bin -File | Where-Object { $_.Name -like 'engine-*' -and $_.Extension -in '.exe', '.pdb', '' })
  $files += @(Get-ChildItem -LiteralPath (Join-Path $bin 'game') -File -Filter '*.dll' -ErrorAction SilentlyContinue)
  $shell = [IO.Path]::GetFullPath((Join-Path $Gate "build/$Preset/shell"))
  if (Test-Path -LiteralPath $shell) { $files += @(Get-ChildItem -LiteralPath $shell -File -Recurse) }
  $done = 0
  foreach ($f in $files) {
    $rel = if ($f.FullName.StartsWith($bin, [StringComparison]::OrdinalIgnoreCase)) { $f.FullName.Substring($bin.Length).TrimStart('\', '/') }
           else { Join-Path 'shell' $f.FullName.Substring($shell.Length).TrimStart('\', '/') }
    $dst = Join-Path $Install $rel
    try {
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
      Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
      if ((Get-FileHash -LiteralPath $dst).Hash -ne (Get-FileHash -LiteralPath $f.FullName).Hash) { throw 'the copy differs from its source' }
      $done++
    } catch { "NOT INSTALLED $rel : $($_.Exception.Message)"; $problems.Add("not installed: $rel") }
  }
  "INSTALL $done of $($files.Count) files into $Install"
}

# 4. The SDK, exported from the gate worktree whose binaries it carries.
if (-not $Sdk) { $Sdk = if ($local) { Join-Path (Join-Path $local 'sdk') $short } else { '' } }
if (-not $Sdk) { $problems.Add('no SDK folder: give -Sdk or set ENGINE_LOCAL_ROOT'); 'SDK skipped: no folder' }
else {
  $code = Start-GateProcess -Script (Join-Path $Gate 'tools/export-game-sdk.ps1') -Arguments @('-Preset', $Preset, '-Out', $Sdk) -Log (Join-Path $logs "$($v.batch)-sdk-export.log") -WorkingDirectory $Gate
  "SDK export exit $code -> $Sdk"
  if ($code -ne 0) { $problems.Add("SDK export exit $code") }
}

# 5. The structure raises, folded on main and committed.
$raises = @(Get-ChildItem -LiteralPath (Join-Path $Main 'tools/structure-raises') -File -Filter '*.json' -ErrorAction SilentlyContinue)
if ($raises.Count -eq 0) { 'FOLD nothing: no raise file on main' }
else {
  $code = Start-GateProcess -Script (Join-Path $Main 'tools/structure-check.ps1') -Arguments @('-Rebaseline') -Log (Join-Path $logs "$($v.batch)-fold.log") -WorkingDirectory $Main
  if ($code -ne 0) { "FOLD failed (exit $code; $logs)"; $problems.Add("fold exit $code") }
  else {
    $add = Invoke-Git $Main add -A -- tools/structure-ceilings.json tools/structure-raises
    $msg = "Fold $($v.batch)'s raises into the structure ceilings"
    if ($Trailer) { $msg += "`n`n$Trailer" }
    $c = Invoke-Git $Main commit -q -m $msg -- tools/structure-ceilings.json tools/structure-raises
    if ($add.Code -ne 0 -or $c.Code -ne 0) { "FOLD not committed: $($add.Out) $($c.Out)"; $problems.Add('fold not committed') }
    else { "FOLD $($raises.Count) raise file(s): $((Invoke-Git $Main log --oneline -1).Out)" }
  }
}

# 6. The push, only when asked.
if ($Push) {
  $p = Invoke-Git $Main push origin main
  if ($p.Code -ne 0) { "PUSH failed: $($p.Out)"; $problems.Add('push') } else { 'PUSH main to origin' }
} else { 'PUSH not taken (-Push not given): main is ahead of origin until someone pushes it' }

if ($problems.Count) { "AFTER exit 1: $($problems -join '; ')"; exit 1 }
'AFTER exit 0'
exit 0
