#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Run a Blender Python script headlessly, outside the repository, and write a provenance sidecar
  for every asset it declares.

.DESCRIPTION
  tools/blender-run.ps1 -Script <file.py> [-Out <dir>] [-Name <run>] [-Blender <exe>]
                        [-LocalRoot <dir>] [-TimeoutMinutes 120] [-GpuLock [-Purpose <text>]]
                        [-DryRun] -- <the script's own arguments>

  Blender is the one DCC tool the content pipeline drives today (experiment E33, the procedural
  ruined-wall kit, docs/experiments/e33-hard-surface-kits.md). This is the one piece of that
  pipeline that lives in the repository; the bpy scripts do not, because the repository takes no
  Python (AGENTS.md), and neither does anything they make. So:

  - The script must live OUTSIDE the repository (this checkout, and from an agent worktree the main
    checkout that contains it) and so must -Out: an output under the tree is one `git add .` from
    being committed, which is the rule tools/generate.ps1 enforces for its outputs. Both are
    refused before Blender starts (exit 2).
  - Blender runs in the background (`-b`) with `--factory-startup --offline-mode
    --disable-autoexec --python-exit-code 1`, and with BLENDER_USER_RESOURCES pointed at
    <local root>/.blender-user, an empty profile of its own. Never the owner's profile and never
    the owner's running Blender: an add-on installed there (the MCP bridge in particular) would
    load, may listen on a socket, and belongs to someone else's session. `--python-exit-code 1`
    makes an exception in the script a non-zero exit, which is what the caller checks.
  - The script's arguments are everything this wrapper does not bind — after `--` from a
    PowerShell session, or simply after the wrapper's own parameters from `pwsh -File`, whose
    binder refuses a bare `--` — passed to Blender after its own `--` untouched; bpy scripts read
    them from `sys.argv` after the `--`.
  - The wrapper sets ENGINE_BLENDER_OUT (the output directory, created) and
    ENGINE_BLENDER_MANIFEST (<Out>/<Name>.manifest.json, deleted before the run so a stale one is
    never read). A script that makes assets writes that manifest:

        {"generator": "ruined-wall", "seed": 7, "parameters": {...},
         "license": "owner-procedural-local",
         "outputs": [{"name": "wall-2m", "path": "wall-2m.glb", "kind": "mesh", "primary": true,
                      "also": ["wall-2m.blend"]}],
         "inputs":  [{"role": "texture", "path": "...", "sha256": "...", "license": "CC0-1.0",
                      "license_url": "...", "source_url": "...", "sidecar": "..."}]}

    and the wrapper writes <name>.provenance.json beside each primary output in the schema
    tools/generate.ps1 writes (`engine.generation.provenance/1`, its `asset_provenance` block
    exactly `engine.content.AssetProvenance`), with the script's SHA-256, its arguments, the seed
    and the parameters as the "prompt", so `tools/generate.ps1 verify -Path <Out>` checks them.
  - stdout is one JSON line per run and nothing else; Blender's own output goes to
    <Out>/<Name>.log, and its last lines to stderr when it fails. Every run appends a line to
    <Out>/runs.jsonl.
  - -GpuLock takes the machine-wide GPU lock for the run (a Cycles GPU bake, a render), or finds it
    held on this run's behalf by `tools/gpu-lock.ps1 run` (ENGINE_GPU_LOCK_HOLDER). Geometry
    generation and CPU work do not need it.
  - -Blender may name a .ps1 file instead of an executable: it is run through this PowerShell with
    the same arguments and environment. That is the test's stand-in for Blender
    (tools/blender-run.Tests.ps1), and nothing else uses it.

  Exit 0 when Blender exited 0 and every declared output exists; 1 when Blender failed, timed out
  or an output is missing; 2 when the run was refused before it started.

.EXAMPLE
  pwsh tools/blender-run.ps1 -Script D:\workspace\game_engine_local\blender-kits\ruined-wall\scripts\ruined_wall.py `
       -Out D:\workspace\game_engine_local\blender-kits\ruined-wall\out\cc0 -Name wall-10m `
       -- --preset wall10m --seed 7 --bond flemish --textures cc0
#>
# The script's own arguments are whatever this one does not bind. In a PowerShell session they
# follow `--`, which ends this script's parameters; through `pwsh -File` a bare `--` is refused by
# PowerShell's own binder ("parameter name '' is ambiguous"), so there they are given after the
# wrapper's parameters without it. Positional binding is off, or `--seed 7` would land in -Name and
# -Blender. One trap remains outside `--`: PowerShell binds `--out x` to -Out as surely as `-Out x`,
# so a bpy script names no option after a parameter of this wrapper (the kit's scripts say `--dest`).
[CmdletBinding(PositionalBinding = $false)]
param(
  [string]$Script,
  [string]$Out,
  [string]$Name,
  [string]$Blender,
  [string]$LocalRoot,
  [double]$TimeoutMinutes = 120,
  [switch]$GpuLock,
  [string]$Purpose,
  [switch]$DryRun,
  [string]$Operator = $(if ($env:ENGINE_OPERATOR) { $env:ENGINE_OPERATOR } elseif ($env:ENGINE_GPU_LOCK_OWNER) { $env:ENGINE_GPU_LOCK_OWNER } else { 'claude-engine' }),
  [Parameter(ValueFromRemainingArguments)] [string[]]$ScriptArgs
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$SidecarSchema = 'engine.generation.provenance/1'
$Sep = [IO.Path]::DirectorySeparatorChar

function Write-Log([string]$text) { [Console]::Error.WriteLine($text) }
function Get-FileSha256([string]$file) { (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() }
function Get-TextSha256([string]$text) {
  ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text)))).ToLowerInvariant()
}
function ConvertTo-Id128([string]$sha256) { $sha256.Substring(0, 32) }
function ConvertTo-U64([string]$sha256) { [UInt64]::Parse($sha256.Substring(0, 16), [Globalization.NumberStyles]::HexNumber) }
function Format-Utc([DateTime]$t) { $t.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
function Get-UnixMs([DateTime]$t) { [DateTimeOffset]::new($t.ToUniversalTime()).ToUnixTimeMilliseconds() }
function Write-JsonFile([string]$file, $value) {
  [IO.File]::WriteAllText($file, ($value | ConvertTo-Json -Depth 40) + "`n", (New-Object Text.UTF8Encoding($false)))
}
# Key order is not something a hash may depend on (the same rule as tools/generate.ps1).
function ConvertTo-Canonical($v) {
  if ($v -is [System.Collections.IDictionary]) {
    $keys = [string[]]@($v.Keys); [Array]::Sort($keys, [StringComparer]::Ordinal)
    $o = [ordered]@{}; foreach ($k in $keys) { $o[$k] = ConvertTo-Canonical $v[$k] }; return $o
  }
  if ($v -is [System.Management.Automation.PSCustomObject]) {
    $keys = [string[]]@($v.PSObject.Properties.Name); [Array]::Sort($keys, [StringComparer]::Ordinal)
    $o = [ordered]@{}; foreach ($k in $keys) { $o[$k] = ConvertTo-Canonical $v.$k }; return $o
  }
  if ($v -is [System.Collections.IList]) { return , @($v | ForEach-Object { ConvertTo-Canonical $_ }) }
  return $v
}
function Get-CanonicalJson($v) { ConvertTo-Canonical $v | ConvertTo-Json -Depth 50 -Compress }

function Stop-Refused([string]$why) {
  Write-Log "blender-run: refused: $why"
  [Console]::Out.WriteLine((@{ status = 'refused'; reason = $why } | ConvertTo-Json -Compress))
  exit 2
}

# The repository roots a path must stay out of: this checkout, and — from an agent worktree under
# <main>/.claude/worktrees/ — the main checkout that contains it (tools/generate.ps1's rule).
function Get-RepoRoots {
  $roots = @($RepoRoot)
  $marker = $Sep + '.claude' + $Sep + 'worktrees' + $Sep
  $at = $RepoRoot.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase)
  if ($at -gt 0) { $roots += $RepoRoot.Substring(0, $at) }
  return $roots
}
function Test-InsideRepo([string]$path) {
  $full = [IO.Path]::GetFullPath($path).TrimEnd('\', '/')
  foreach ($r in Get-RepoRoots) {
    $root = $r.TrimEnd('\', '/')
    if ($full -ieq $root -or $full.StartsWith($root + $Sep, [StringComparison]::OrdinalIgnoreCase)) { return $root }
  }
  return $null
}

function Get-LocalRoot {
  $root = if ($LocalRoot) { $LocalRoot } elseif ($env:ENGINE_LOCAL_ROOT) { $env:ENGINE_LOCAL_ROOT } elseif ($IsWin) { 'D:\workspace\game_engine_local' } else { Join-Path $HOME 'game_engine_local' }
  $full = [IO.Path]::GetFullPath($root)
  $inside = Test-InsideRepo $full
  if ($inside) { Stop-Refused "local root '$full' is inside the repository ($inside); point -LocalRoot or ENGINE_LOCAL_ROOT outside it" }
  return $full
}

function Find-Blender {
  if ($Blender) { return [IO.Path]::GetFullPath($Blender) }
  if ($env:BLENDER_EXE) { return $env:BLENDER_EXE }
  if ($IsWin) {
    $base = Join-Path $env:ProgramFiles 'Blender Foundation'
    $found = @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
               Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'blender.exe') } |
               Sort-Object { [version]($_.Name -replace '^[^\d]*', '' -replace '[^\d.].*$', '') } -Descending)
    if ($found.Count -gt 0) { return (Join-Path $found[0].FullName 'blender.exe') }
  } else {
    $cmd = Get-Command blender -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
  }
  Stop-Refused 'Blender not found; pass -Blender <blender executable> or set BLENDER_EXE'
}

function Get-ToolInfo {
  $commit = ''; $dirty = $false
  try {
    $commit = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    $dirty = -not [string]::IsNullOrWhiteSpace((& git -C $RepoRoot status --porcelain -- tools 2>$null | Out-String))
  } catch { }
  return [ordered]@{ script = 'tools/blender-run.ps1'; commit = $commit; tools_dirty = $dirty }
}

# ---- what to run ------------------------------------------------------------------------------------

# A `--` that reached $args (a caller that quoted it) is the separator, not an argument.
$passArgs = @($ScriptArgs | Where-Object { $null -ne $_ } | ForEach-Object { [string]$_ })
if ($passArgs.Count -gt 0 -and $passArgs[0] -eq '--') { $passArgs = @($passArgs | Select-Object -Skip 1) }

if (-not $Script) { Stop-Refused 'no -Script <file.py> given' }
if (-not (Test-Path -LiteralPath $Script -PathType Leaf)) { Stop-Refused "no script at '$Script'" }
$ScriptFull = (Resolve-Path -LiteralPath $Script).Path
$inside = Test-InsideRepo $ScriptFull
if ($inside) {
  Stop-Refused "script '$ScriptFull' is inside the repository ($inside). The repository takes no Python: bpy scripts live under the local root (docs/content-generation.md, 'Blender')"
}
$root = Get-LocalRoot
if (-not $Name) { $Name = [IO.Path]::GetFileNameWithoutExtension($ScriptFull) }
if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { Stop-Refused "name '$Name' must be letters, digits, dots, hyphens and underscores" }
if (-not $Out) { $Out = Join-Path (Join-Path (Join-Path $root 'blender-runs') ([IO.Path]::GetFileNameWithoutExtension($ScriptFull))) ([DateTime]::Now.ToString('yyyy-MM-dd')) }
$OutFull = [IO.Path]::GetFullPath($Out)
$inside = Test-InsideRepo $OutFull
if ($inside) { Stop-Refused "output directory '$OutFull' is inside the repository ($inside). Blender outputs are never committed; put them under the local root" }

$exe = Find-Blender
$isStub = $exe -match '\.ps1$'
$profileDir = Join-Path $root '.blender-user'
$manifest = Join-Path $OutFull "$Name.manifest.json"
$log = Join-Path $OutFull "$Name.log"
$blenderArgs = @('-b', '--factory-startup', '--offline-mode', '--disable-autoexec', '--python-exit-code', '1', '--python', $ScriptFull, '--') + $passArgs
$scriptSha = Get-FileSha256 $ScriptFull

if ($DryRun) {
  [Console]::Out.WriteLine(([ordered]@{
    status = 'dry-run'; blender = $exe; args = $blenderArgs; out = $OutFull; manifest = $manifest; log = $log
    env = [ordered]@{ BLENDER_USER_RESOURCES = $profileDir; ENGINE_BLENDER_OUT = $OutFull; ENGINE_BLENDER_MANIFEST = $manifest }
    script_sha256 = $scriptSha; gpu_lock = [bool]$GpuLock
  } | ConvertTo-Json -Depth 5 -Compress))
  exit 0
}

New-Item -ItemType Directory -Force -Path $OutFull, $profileDir | Out-Null
if (Test-Path -LiteralPath $manifest) { Remove-Item -LiteralPath $manifest -Force }

# The Blender version goes into every sidecar; a stand-in has none.
$blenderVersion = $null
if (-not $isStub) {
  $v = (& $exe --factory-startup --version 2>$null | Select-Object -First 1)
  if ($v) { $blenderVersion = ([string]$v).Trim() }
}

# ---- the run ----------------------------------------------------------------------------------------

$lockSession = $null
if ($GpuLock) {
  Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force
  $held = Read-MachineLock -Path (Get-MachineLockPath -Kind gpu)
  if ($env:ENGINE_GPU_LOCK_HOLDER -and $held.Present -and -not $held.Expired -and [string]$held.Pid -eq [string]$env:ENGINE_GPU_LOCK_HOLDER) {
    Write-Log "blender-run: GPU lock held on this run's behalf by pid $($held.Pid)"
  } else {
    $why = if ($Purpose) { $Purpose } else { "blender-run $Name ($([IO.Path]::GetFileName($ScriptFull)))" }
    $lockSession = Open-MachineLockSession -Kind gpu -Purpose $why
  }
}

$started = [DateTime]::UtcNow
$exitCode = -1
$timedOut = $false
try {
  $psi = [Diagnostics.ProcessStartInfo]::new()
  if ($isStub) {
    $psi.FileName = (Get-Process -Id $PID).Path
    foreach ($a in @('-NoProfile', '-NonInteractive', '-File', $exe)) { $psi.ArgumentList.Add($a) }
  } else {
    $psi.FileName = $exe
  }
  foreach ($a in $blenderArgs) { $psi.ArgumentList.Add([string]$a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.WorkingDirectory = $OutFull
  $psi.Environment['BLENDER_USER_RESOURCES'] = $profileDir
  $psi.Environment['ENGINE_BLENDER_OUT'] = $OutFull
  $psi.Environment['ENGINE_BLENDER_MANIFEST'] = $manifest
  Write-Log "blender-run: $([IO.Path]::GetFileName($ScriptFull)) $($passArgs -join ' ')  (log: $log)"
  $proc = [Diagnostics.Process]::Start($psi)
  $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
  $stderrTask = $proc.StandardError.ReadToEndAsync()
  if (-not $proc.WaitForExit([int][math]::Min([int]::MaxValue, $TimeoutMinutes * 60000))) {
    $timedOut = $true
    try { $proc.Kill($true) } catch { }
    $proc.WaitForExit()
  }
  $exitCode = if ($timedOut) { -1 } else { $proc.ExitCode }
  $text = $stdoutTask.GetAwaiter().GetResult()
  $errText = $stderrTask.GetAwaiter().GetResult()
  [IO.File]::WriteAllText($log, $text + $(if ($errText) { "`n--- stderr ---`n" + $errText } else { '' }), (New-Object Text.UTF8Encoding($false)))
} finally {
  if ($lockSession) { Close-MachineLockSession -Session $lockSession }
}
$finished = [DateTime]::UtcNow

# ---- what it made -----------------------------------------------------------------------------------

$problems = [Collections.Generic.List[string]]::new()
if ($timedOut) { $problems.Add("timed out after $TimeoutMinutes minutes") }
elseif ($exitCode -ne 0) { $problems.Add("Blender exited $exitCode") }

$sidecars = [Collections.Generic.List[string]]::new()
$outputs = [Collections.Generic.List[object]]::new()
$m = $null
if ($exitCode -eq 0 -and (Test-Path -LiteralPath $manifest)) {
  try { $m = Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json } catch { $problems.Add("manifest does not parse: $($_.Exception.Message)") }
}
if ($m) {
  $inputs = @()
  $inputs += [ordered]@{ role = 'script'; path = $ScriptFull; sha256 = $scriptSha; bytes = (Get-Item -LiteralPath $ScriptFull).Length }
  foreach ($i in @($m.inputs)) {
    if ($null -eq $i) { continue }
    $rec = [ordered]@{}
    foreach ($p in $i.PSObject.Properties) { $rec[$p.Name] = $p.Value }
    if (-not $rec.Contains('sha256') -and $rec.Contains('path') -and (Test-Path -LiteralPath $rec.path)) { $rec.sha256 = Get-FileSha256 $rec.path }
    if (-not $rec.Contains('sha256')) { $problems.Add("input without a hash or a readable path: $($rec | ConvertTo-Json -Compress)"); continue }
    $inputs += $rec
  }
  # An input with a sidecar of its own is derived-from by its asset id; anything else by the first
  # 128 bits of its SHA-256, which is how tools/generate.ps1 records an input image.
  $derived = @($inputs | Where-Object { $_.role -ne 'script' } | ForEach-Object {
    $id = $null
    if ($_.Contains('sidecar') -and $_.sidecar -and (Test-Path -LiteralPath $_.sidecar)) {
      try { $id = (Get-Content -Raw -LiteralPath $_.sidecar | ConvertFrom-Json).asset_id } catch { }
    }
    if ($id) { $id } else { ConvertTo-Id128 $_.sha256 }
  })
  $parameters = $m.parameters
  $seed = if ($null -ne $m.seed) { [long]$m.seed } else { 0 }
  # The "prompt" of a procedural asset is the generator, the script's bytes, its arguments, the
  # seed and the parameters; its hash is what makes two runs the same run.
  $promptText = "$([IO.Path]::GetFileName($ScriptFull)) sha256:$scriptSha -- $($passArgs -join ' ')"
  $spec = [ordered]@{ script_sha256 = $scriptSha; args = @($passArgs); seed = $seed; parameters = $parameters; inputs = @($inputs | Where-Object { $_.role -ne 'script' } | ForEach-Object { $_.sha256 }); blender = $blenderVersion }
  $specHash = Get-TextSha256 (Get-CanonicalJson $spec)
  $licence = if ($m.license) { [string]$m.license } else { 'owner-procedural-local' }
  foreach ($o in @($m.outputs)) {
    if ($null -eq $o) { continue }
    $file = if ([IO.Path]::IsPathRooted([string]$o.path)) { [string]$o.path } else { Join-Path $OutFull ([string]$o.path) }
    if (-not (Test-Path -LiteralPath $file)) { $problems.Add("declared output missing: $file"); continue }
    $recs = @([ordered]@{ role = $(if ($o.kind) { [string]$o.kind } else { 'output' }); path = [IO.Path]::GetFileName($file); sha256 = Get-FileSha256 $file; bytes = (Get-Item -LiteralPath $file).Length })
    foreach ($extra in @($o.also)) {
      if (-not $extra) { continue }
      $ef = if ([IO.Path]::IsPathRooted([string]$extra)) { [string]$extra } else { Join-Path $OutFull ([string]$extra) }
      if (-not (Test-Path -LiteralPath $ef)) { $problems.Add("declared output missing: $ef"); continue }
      $recs += [ordered]@{ role = [IO.Path]::GetExtension($ef).TrimStart('.'); path = [IO.Path]::GetFileName($ef); sha256 = Get-FileSha256 $ef; bytes = (Get-Item -LiteralPath $ef).Length }
    }
    $outputs.Add([ordered]@{ name = [string]$o.name; path = $file })
    if (-not $o.primary) { continue }
    $outName = if ($o.name) { [string]$o.name } else { [IO.Path]::GetFileNameWithoutExtension($file) }
    $sidecar = [ordered]@{
      schema = $SidecarSchema
      asset_id = ConvertTo-Id128 $recs[0].sha256
      name = $outName
      kind = $(if ($o.kind) { [string]$o.kind } else { 'mesh' })
      service = 'blender'
      spec_hash = $specHash
      asset_provenance = [ordered]@{
        generator = "blender/$([IO.Path]::GetFileName($ScriptFull))"
        model_id = $null
        model_version = $null
        prompt_hash = ConvertTo-U64 $specHash
        seed = [UInt64][math]::Max(0, $seed)
        inputs = @($inputs | ForEach-Object { ConvertTo-Id128 $_.sha256 })
        operator_id = ConvertTo-Id128 (Get-TextSha256 "operator:$Operator")
        created_at_unix_ms = Get-UnixMs $finished
        license = $licence
        license_class = 'Proprietary'
        license_terms_url = $null
        commercial_ok = $false
        attribution_required = $true
        derived_from = @($derived)
      }
      prompt = [ordered]@{ kind = 'procedural'; generator = [string]$m.generator; script = [IO.Path]::GetFileName($ScriptFull); script_sha256 = $scriptSha; args = @($passArgs); text = $promptText; negative = $null }
      seed = $seed
      parameters = $parameters
      backend = [ordered]@{ blender = [ordered]@{ executable = $exe; version = $blenderVersion; flags = @($blenderArgs | Select-Object -First 8) }; script = [ordered]@{ path = $ScriptFull; sha256 = $scriptSha }; manifest = [IO.Path]::GetFileName($manifest); log = [IO.Path]::GetFileName($log) }
      inputs = @($inputs)
      outputs = @($recs)
      credits = $null
      operator = $Operator
      started_utc = Format-Utc $started
      finished_utc = Format-Utc $finished
      seconds = [math]::Round(($finished - $started).TotalSeconds, 1)
      license = $licence
      content_class = 'general'
      content_classes = [ordered]@{ nudity = 'none'; sexual = 'none'; violence = 'none' }
      tool = Get-ToolInfo
    }
    $path = Join-Path (Split-Path -Parent $file) "$outName.provenance.json"
    Write-JsonFile $path $sidecar
    $sidecars.Add($path)
  }
} elseif ($exitCode -eq 0) {
  Write-Log "blender-run: the script wrote no manifest ($manifest), so no sidecar was written"
}

$ok = $problems.Count -eq 0
$record = [ordered]@{
  status = $(if ($ok) { 'ok' } else { 'failed' })
  name = $Name
  script = $ScriptFull
  script_sha256 = $scriptSha
  args = @($passArgs)
  blender = $blenderVersion
  exit_code = $exitCode
  seconds = [math]::Round(($finished - $started).TotalSeconds, 1)
  started_utc = Format-Utc $started
  out = $OutFull
  log = $log
  outputs = @($outputs)
  sidecars = @($sidecars)
  problems = @($problems)
}
[IO.File]::AppendAllText((Join-Path $OutFull 'runs.jsonl'), ($record | ConvertTo-Json -Depth 10 -Compress) + "`n", (New-Object Text.UTF8Encoding($false)))
if (-not $ok) {
  foreach ($p in $problems) { Write-Log "blender-run: $p" }
  if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 30 | ForEach-Object { Write-Log "  | $_" } }
}
[Console]::Out.WriteLine(($record | ConvertTo-Json -Depth 10 -Compress))
exit $(if ($ok) { 0 } else { 1 })
