#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/blender-run.ps1. Runs under CTest as `tools.blender_run`.

.DESCRIPTION
  Offline and without Blender: the wrapper's `-Blender` accepts a .ps1 stand-in, which this test
  writes into a scratch directory. It checks that

  - a script, an output directory or a local root inside the repository is refused (exit 2) before
    anything runs, since the repository takes no Python and no generated asset;
  - the command line Blender would get is background, factory settings, offline, no auto-run
    scripts, a non-zero exit on a Python error, and the script's own arguments after its `--`
    verbatim, whether they came after `--` in a session or straight after the wrapper's parameters
    through `pwsh -File`;
  - Blender's environment names an isolated profile (never the owner's), the output directory and
    the manifest path;
  - a run that declares an output gets a provenance sidecar in tools/generate.ps1's schema — the
    service, the generator, the script's hash, the seed and parameters as the prompt, the inputs,
    every output's hash — which `tools/generate.ps1 verify` accepts (against the JSON Schema the
    build generates, when this checkout has built one);
  - a failing script, and a declared output that is not there, exit 1 and write no sidecar; and
    every run appends a line to runs.jsonl.

      pwsh tools/blender-run.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
$tool = Join-Path $PSScriptRoot 'blender-run.ps1'
$generate = Join-Path $PSScriptRoot 'generate.ps1'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pwshPath = [Environment]::ProcessPath
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

function Invoke-Tool {
  $errFile = [IO.Path]::GetTempFileName()
  try {
    $out = & $pwshPath -NoProfile -NonInteractive -File $tool @args 2> $errFile
    $code = $LASTEXITCODE
    $err = Get-Content -Raw -LiteralPath $errFile
  } finally { Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue }
  $json = @($out | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } | ForEach-Object { try { $_ | ConvertFrom-Json } catch { } })
  return [pscustomobject]@{ Code = $code; Out = @($out); Json = $json; Err = "$err" }
}

$root = Join-Path ([IO.Path]::GetTempPath()) "engine-blender-run-$([guid]::NewGuid().ToString('N'))"
$local = Join-Path $root 'local'
$scripts = Join-Path $local 'scripts'
New-Item -ItemType Directory -Force -Path $scripts | Out-Null
$script = Join-Path $scripts 'kit.py'
Set-Content -LiteralPath $script -Value "# a bpy script the stand-in never runs`n" -NoNewline
$texture = Join-Path $local 'brick.jpg'
[IO.File]::WriteAllBytes($texture, [byte[]](1..64))

# The stand-in for Blender: records what it was given, then behaves as the arguments after `--` say.
$stub = Join-Path $root 'blender-stub.ps1'
@'
$dd = [Array]::IndexOf($args, '--')
$mine = if ($dd -ge 0) { @($args | Select-Object -Skip ($dd + 1)) } else { @() }
$record = [ordered]@{ args = @($args); profile = $env:BLENDER_USER_RESOURCES; out = $env:ENGINE_BLENDER_OUT; manifest = $env:ENGINE_BLENDER_MANIFEST }
[IO.File]::WriteAllText((Join-Path $env:ENGINE_BLENDER_OUT 'stub-record.json'), ($record | ConvertTo-Json -Depth 5))
Write-Output 'stub: fake blender output line'
if ($mine -contains '--fail') { [Console]::Error.WriteLine('Traceback: the script failed'); exit 3 }
$glb = Join-Path $env:ENGINE_BLENDER_OUT 'wall.glb'
[IO.File]::WriteAllBytes($glb, [Text.Encoding]::ASCII.GetBytes('glTF fake mesh ' + ($mine -join ' ')))
[IO.File]::WriteAllText((Join-Path $env:ENGINE_BLENDER_OUT 'wall.blend'), 'fake blend')
$outs = @([ordered]@{ name = 'wall'; path = 'wall.glb'; kind = 'mesh'; primary = $true; also = @('wall.blend') })
if ($mine -contains '--declare-missing') { $outs += [ordered]@{ name = 'ghost'; path = 'ghost.glb'; kind = 'mesh'; primary = $true } }
$m = [ordered]@{ generator = 'ruined-wall'; seed = 7; parameters = [ordered]@{ preset = 'section2m'; bond = 'flemish' }
  license = 'owner-procedural-local'; outputs = $outs
  inputs = @([ordered]@{ role = 'texture'; path = $env:E33_TEST_TEXTURE; license = 'CC0-1.0'; source_url = 'https://example.invalid/brick.jpg' }) }
[IO.File]::WriteAllText($env:ENGINE_BLENDER_MANIFEST, ($m | ConvertTo-Json -Depth 6))
exit 0
'@ | Set-Content -LiteralPath $stub
$env:E33_TEST_TEXTURE = $texture

try {
  Write-Host 'refusals'
  $r = Invoke-Tool -Script (Join-Path $repo 'tools/dev.ps1') -Blender $stub -LocalRoot $local --x
  Test-That 'a script inside the repository is refused with exit 2' { $r.Code -eq 2 -and $r.Json[0].status -eq 'refused' -and $r.Json[0].reason -match 'inside the repository' }
  $r = Invoke-Tool -Script $script -Blender $stub -LocalRoot $local -Out (Join-Path $repo 'build/blender-run-test') --x
  Test-That 'an output directory inside the repository is refused with exit 2' { $r.Code -eq 2 -and $r.Json[0].reason -match 'output directory' }
  Test-That '... and nothing was created there' { -not (Test-Path (Join-Path $repo 'build/blender-run-test')) }
  $r = Invoke-Tool -Script $script -Blender $stub -LocalRoot (Join-Path $repo 'x-local') --x
  Test-That 'a local root inside the repository is refused with exit 2' { $r.Code -eq 2 -and $r.Json[0].reason -match 'local root' }
  $r = Invoke-Tool -Script (Join-Path $scripts 'missing.py') -Blender $stub -LocalRoot $local
  Test-That 'a missing script is refused with exit 2' { $r.Code -eq 2 }

  Write-Host 'the command line (dry run)'
  $out1 = Join-Path $local 'out1'
  $r = Invoke-Tool -Script $script -Blender $stub -LocalRoot $local -Out $out1 -DryRun --preset section2m --seed 7 --dest 'a b'
  $d = $r.Json[0]
  Test-That 'a dry run exits 0 and runs nothing' { $r.Code -eq 0 -and $d.status -eq 'dry-run' -and -not (Test-Path $out1) }
  Test-That 'Blender runs in the background with factory settings, offline, no auto-run scripts, exit code on error' {
    ($d.args[0..7] -join ' ') -eq "-b --factory-startup --offline-mode --disable-autoexec --python-exit-code 1 --python $script"
  }
  Test-That 'the script arguments follow `--` verbatim (through pwsh -File, without a separator)' { ($d.args[8..13] -join '|') -eq '--|--preset|section2m|--seed|7|--dest' -and $d.args[14] -eq 'a b' }
  Test-That 'the profile is isolated under the local root' { $d.env.BLENDER_USER_RESOURCES -eq (Join-Path $local '.blender-user') }
  # "In a session": the wrapper called with `&` from PowerShell code, where `--` is the separator.
  $cmd = "& '$tool' -Script '$script' -Blender '$stub' -LocalRoot '$local' -Out '$out1' -DryRun -- --preset corner -DryRun"
  $inproc = (& $pwshPath -NoProfile -NonInteractive -Command $cmd 2>$null) | Where-Object { $_ -match '^\{' } | ConvertFrom-Json
  Test-That 'in a session, everything after `--` is the script''s, even a name this wrapper has' { ($inproc.args[8..11] -join '|') -eq '--|--preset|corner|-DryRun' }

  Write-Host 'a run through the stand-in'
  $r = Invoke-Tool -Script $script -Blender $stub -LocalRoot $local -Out $out1 -Name wall-run --preset section2m --seed 7
  $res = $r.Json[0]
  Test-That 'the run exits 0 and reports ok' { $r.Code -eq 0 -and $res.status -eq 'ok' }
  $rec = Get-Content -Raw (Join-Path $out1 'stub-record.json') | ConvertFrom-Json
  Test-That 'Blender got the script arguments after its --' { (@($rec.args) -join ' ') -match '-- --preset section2m --seed 7$' }
  Test-That 'Blender''s environment names the output directory and the manifest' { $rec.out -eq $out1 -and $rec.manifest -eq (Join-Path $out1 'wall-run.manifest.json') }
  Test-That 'Blender''s output is in the log' { (Get-Content -Raw (Join-Path $out1 'wall-run.log')) -match 'fake blender output line' }
  $scPath = Join-Path $out1 'wall.provenance.json'
  Test-That 'a sidecar is written beside the declared output' { Test-Path $scPath }
  $sc = Get-Content -Raw $scPath | ConvertFrom-Json
  $glbSha = (Get-FileHash -Algorithm SHA256 (Join-Path $out1 'wall.glb')).Hash.ToLowerInvariant()
  $scriptSha = (Get-FileHash -Algorithm SHA256 $script).Hash.ToLowerInvariant()
  $texSha = (Get-FileHash -Algorithm SHA256 $texture).Hash.ToLowerInvariant()
  Test-That 'the sidecar has generate.ps1''s schema, service blender and a general content class' { $sc.schema -eq 'engine.generation.provenance/1' -and $sc.service -eq 'blender' -and $sc.content_class -eq 'general' -and $sc.kind -eq 'mesh' }
  Test-That 'asset_id is the mesh''s content id' { $sc.asset_id -eq $glbSha.Substring(0, 32) }
  Test-That 'the prompt is the generator, the script''s hash and its arguments' { $sc.prompt.kind -eq 'procedural' -and $sc.prompt.script_sha256 -eq $scriptSha -and (@($sc.prompt.args) -join ' ') -eq '--preset section2m --seed 7' }
  Test-That 'the seed and the parameters are recorded' { $sc.seed -eq 7 -and $sc.parameters.bond -eq 'flemish' -and $sc.asset_provenance.seed -eq 7 }
  Test-That 'asset_provenance names the generator by script, no model, and the conservative licence defaults' {
    $sc.asset_provenance.generator -eq 'blender/kit.py' -and $null -eq $sc.asset_provenance.model_id -and $sc.asset_provenance.commercial_ok -eq $false -and $sc.asset_provenance.attribution_required -eq $true -and $sc.asset_provenance.license -eq 'owner-procedural-local'
  }
  Test-That 'the inputs are the script and the texture, each hashed, and the texture keeps its licence' {
    $sc.inputs[0].role -eq 'script' -and $sc.inputs[0].sha256 -eq $scriptSha -and $sc.inputs[1].sha256 -eq $texSha -and $sc.inputs[1].license -eq 'CC0-1.0'
  }
  Test-That 'asset_provenance.inputs are content ids of both, derived_from the texture''s' { (@($sc.asset_provenance.inputs) -join ',') -eq "$($scriptSha.Substring(0, 32)),$($texSha.Substring(0, 32))" -and (@($sc.asset_provenance.derived_from) -join ',') -eq $texSha.Substring(0, 32) }
  Test-That 'every output is hashed, the .blend beside the mesh too' { @($sc.outputs).Count -eq 2 -and $sc.outputs[1].path -eq 'wall.blend' -and $sc.outputs[0].sha256 -eq $glbSha }
  Test-That 'the spec hash is a SHA-256 and prompt_hash its first 64 bits' { $sc.spec_hash -match '^[0-9a-f]{64}$' -and [UInt64]$sc.asset_provenance.prompt_hash -eq [UInt64]::Parse($sc.spec_hash.Substring(0, 16), 'HexNumber') }
  $v = & $pwshPath -NoProfile -NonInteractive -File $generate verify -Path $out1 2>$null
  $vcode = $LASTEXITCODE
  $vj = @($v | Where-Object { $_ -match '^\{' } | ForEach-Object { $_ | ConvertFrom-Json })
  Test-That 'tools/generate.ps1 verify accepts the sidecar' { $vcode -eq 0 -and $vj[0].status -eq 'ok' -and $vj[0].sidecars -eq 1 }
  Test-That 'the run is a line of runs.jsonl' { @(Get-Content (Join-Path $out1 'runs.jsonl')).Count -eq 1 }

  Write-Host 'failures'
  $out2 = Join-Path $local 'out2'
  $r = Invoke-Tool -Script $script -Blender $stub -LocalRoot $local -Out $out2 -Name bad --fail
  Test-That 'a failing script exits 1 with its exit code in the result' { $r.Code -eq 1 -and $r.Json[0].status -eq 'failed' -and $r.Json[0].exit_code -eq 3 }
  Test-That '... writes no sidecar, and shows the end of its log' { -not (Get-ChildItem $out2 -Filter *.provenance.json) -and $r.Err -match 'Traceback' }
  $out3 = Join-Path $local 'out3'
  $r = Invoke-Tool -Script $script -Blender $stub -LocalRoot $local -Out $out3 -Name ghost --declare-missing
  Test-That 'a declared output that is not there fails the run' { $r.Code -eq 1 -and (@($r.Json[0].problems) -join ' ') -match 'ghost.glb' }
  Test-That 'a failed run is still a line of runs.jsonl' { @(Get-Content (Join-Path $out2 'runs.jsonl')).Count -eq 1 }
} finally {
  Remove-Item Env:E33_TEST_TEXTURE -ErrorAction SilentlyContinue
  if ($KeepTemp -or $failures.Count -gt 0) { Write-Host "scratch kept: $root" }
  else { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "$checks checks, $($failures.Count) failed"
if ($failures.Count -gt 0) { exit 1 }
exit 0
