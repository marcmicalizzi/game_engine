#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Searches the engine's own translation units for the signature of the MSVC by-value temporary
  miscompile (docs/experiments/msvc-ob2-avx2-miscompile.md): a call given the address of a copy
  that nothing wrote.

.DESCRIPTION
  tools/miscompile-scan.ps1 [-Preset msvc-release] [-Level Ob1,Ob2] [-Filter <regex>] [-Jobs 8]
                            [-Out <dir>] [-Keep] [-Switch /d2ssa-cse-]

  For every C++ unit of build/<preset>/compile_commands.json that is the engine's own (libraries,
  tests, benches, apps, tools and generated sources; nothing vendored or fetched) and whose path
  matches -Filter, at each inlining level asked: the unit is compiled twice to an assembly
  listing with the tree's own compile line, once as it is and once with -Switch (the optimizer
  pass whose absence makes the reduction right), and the two listings are compared by
  tools/lib/MiscompileScan.psm1. A unit is a CANDIDATE when the plain listing passes a call the
  address of more unwritten temporaries than the switched one does, the call is not one that
  fills the temporary itself, and the site is not on tools/miscompile-scan-read.txt, the sites
  already read. A candidate is something to read, not a verdict: the page says how ("Reading a
  candidate").

  The preset must be configured and built first: the units include generated headers. Nothing in
  the build tree but -Out is written (default build/<preset>/miscompile-scan/). Listings go
  there, each removed as soon as it is read, with the object its compile has to write (no debug
  information in it); -Keep leaves a candidate's two listings for reading. The reports are
  <out>/report.<level>.txt, one line a unit, and <out>/report.<level>.json; the summary says how
  many bytes were written and removed, which for the whole tree is about 9.6 GB a level.

  Windows and MSVC only: the fault is that compiler's. When it runs: docs/ci/what-to-run.md, "The
  miscompile scan".
  Exit codes: 0 no candidate; 1 a candidate to read; 2 a unit did not compile, or nothing to scan.
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [string[]]$Level = @('Ob1', 'Ob2'),
  [string]$Filter = '',
  [int]$Jobs = 8,
  [string]$Out = '',
  [switch]$Keep,
  [string]$Switch = '/d2ssa-cse-'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$module = Join-Path $PSScriptRoot 'lib/MiscompileScan.psm1'
Import-Module $module -Force
if (-not ($IsWindows -or $env:OS -eq 'Windows_NT')) { Write-Host 'miscompile-scan: Windows and MSVC only' -ForegroundColor Red; exit 2 }
$Level = @($Level | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($l in $Level) { if ($l -notmatch '^Ob[0-3]$') { Write-Host "miscompile-scan: a level is Ob0 to Ob3, not '$l'" -ForegroundColor Red; exit 2 } }

$buildDir = Join-Path $Root "build/$Preset"
$commands = Join-Path $buildDir 'compile_commands.json'
if (-not (Test-Path $commands)) { Write-Host "miscompile-scan: no $commands; configure and build the preset first" -ForegroundColor Red; exit 2 }
if (-not $Out) { $Out = Join-Path $buildDir 'miscompile-scan' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# The compiler's environment, as tools/dev.ps1 finds it: skipped when this shell already has one.
if (-not (Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { Write-Host 'miscompile-scan: no cl.exe on PATH and no vswhere to find one' -ForegroundColor Red; exit 2 }
  $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $env:PATH = "$(Split-Path $vswhere);$env:PATH"
  $envDump = & cmd.exe /d /c "`"$(Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat')`" -arch=x64 -host_arch=x64 -no_logo && set"
  foreach ($line in $envDump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] } }
}
$banner = (& cmd.exe /d /c 'cl.exe 2>&1' | Select-Object -First 1)
$readPath = Join-Path $PSScriptRoot 'miscompile-scan-read.txt'
$readList = @(if (Test-Path $readPath) { Read-ScanReadList -Lines @(Get-Content $readPath) })

$rootSlash = $Root.Replace('\', '/').TrimEnd('/')
$n = 0
$units = @(Get-Content $commands -Raw | ConvertFrom-Json | ForEach-Object {
    $file = $_.file.Replace('\', '/')
    $rel = if ($file.StartsWith("$rootSlash/", [StringComparison]::OrdinalIgnoreCase)) { $file.Substring($rootSlash.Length + 1) } else { $file }
    if (Test-ScanUnit -Relative $rel -Filter $Filter) { [pscustomobject]@{ Rel = $rel; Command = $_.command; Directory = $_.directory } }
  } | Sort-Object Rel | ForEach-Object { $_ | Add-Member -NotePropertyName N -NotePropertyValue (++$n) -PassThru })
if ($units.Count -eq 0) { Write-Host 'miscompile-scan: no unit matches' -ForegroundColor Red; exit 2 }
Write-Host "miscompile-scan: $($units.Count) units, levels $($Level -join ', '), switch $Switch, $Jobs jobs"
Write-Host "  compiler: $banner"

$worst = 0
foreach ($lvl in $Level) {
  $work = Join-Path $Out $lvl
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  $started = Get-Date
  # Each unit's result comes back as one object and the report is written once, here: appends from
  # eight jobs to one file lose lines.
  $results = @($units | ForEach-Object -ThrottleLimit $Jobs -Parallel {
      $u = $_
      Import-Module $using:module
      $a = Join-Path $using:work "$($u.N).plain.asm"; $b = Join-Path $using:work "$($u.N).switched.asm"
      $r = [pscustomobject]@{ Rel = $u.Rel; N = $u.N; State = 'clean'; Bytes = 0L; Sites = @(); Error = '' }
      try {
        $c = Get-ScanCommands -Command $u.Command -Level $using:lvl -PlainListing $a -SwitchedListing $b -Switch $using:Switch
        # The arguments through a response file: an app's compile line is longer than a command shell takes.
        [IO.File]::WriteAllText("$a.rsp", $c.PlainArguments); [IO.File]::WriteAllText("$b.rsp", $c.SwitchedArguments)
        Push-Location $u.Directory
        try {
          $oa = & $c.Compiler "@$a.rsp" 2>&1; $ea = $LASTEXITCODE
          $ob = & $c.Compiler "@$b.rsp" 2>&1; $eb = $LASTEXITCODE
        } finally { Pop-Location; Remove-Item "$a.rsp", "$b.rsp" -ErrorAction SilentlyContinue }
        foreach ($f in $a, $b, "$a.obj", "$b.obj") { if (Test-Path $f) { $r.Bytes += (Get-Item $f).Length } }
        if ($ea -ne 0 -or $eb -ne 0 -or -not (Test-Path $a) -or -not (Test-Path $b)) {
          $r.State = 'failed'
          $r.Error = "$((@($oa) + @($ob) | ForEach-Object { "$_" } | Where-Object { $_ -match 'error' } | Select-Object -First 1))"
          if (-not $r.Error) { $r.Error = "exit $ea and $eb, and no error line: $((@($oa) + @($ob) | Select-Object -Last 1))" }
        } else {
          $x = Get-AddressOnlyTemps -Lines ([IO.File]::ReadLines($a))
          $y = Get-AddressOnlyTemps -Lines ([IO.File]::ReadLines($b))
          $r.Sites = @(Compare-AddressOnlyTemps -Plain $x -Switched $y)
          if (@($r.Sites | Where-Object Class -eq 'candidate').Count) { $r.State = 'candidate' }
        }
      } catch { $r.State = 'failed'; $r.Error = "$_" }
      Remove-Item "$a.obj", "$b.obj" -ErrorAction SilentlyContinue
      if (-not ($using:Keep -and $r.State -eq 'candidate')) { Remove-Item $a, $b -ErrorAction SilentlyContinue }
      $r
    })
  $seconds = [int]((Get-Date) - $started).TotalSeconds
  $results = @($results | Sort-Object Rel)
  # Sites already read (tools/miscompile-scan-read.txt) are reported as such and are not candidates.
  foreach ($r in $results) {
    if ($r.State -ne 'candidate') { continue }
    foreach ($s in $r.Sites) {
      if ($s.Class -ne 'candidate') { continue }
      $e = Find-ScanReadEntry -Entries $readList -Unit $r.Rel -Place $s.Register -Callee $s.Callee
      if ($e) { $e.Used = $true; $s.Class = 'read'; $s | Add-Member -NotePropertyName Reason -NotePropertyValue $e.Reason -Force }
    }
    if (-not @($r.Sites | Where-Object Class -eq 'candidate').Count) { $r.State = 'read' }
  }
  $candidates = @($results | Where-Object State -eq 'candidate')
  $failed = @($results | Where-Object State -eq 'failed')
  $bytes = ($results | Measure-Object -Property Bytes -Sum).Sum
  $aside = @{}
  foreach ($r in $results) { foreach ($s in $r.Sites) { if ($s.Class -ne 'candidate') { $aside[$s.Class] = $s.Count + [int]$aside[$s.Class] } } }
  $lines = [System.Collections.Generic.List[string]]::new()
  $lines.Add("miscompile-scan at /$lvl against $Switch, preset $Preset, $($units.Count) units, $seconds s at $Jobs jobs")
  $lines.Add("compiler: $banner")
  $read = @($results | Where-Object State -eq 'read')
  $summary = "clean $($results.Count - $candidates.Count - $failed.Count - $read.Count), read before $($read.Count), candidates $($candidates.Count), did not compile $($failed.Count); listings and objects written and removed: $([Math]::Round($bytes / 1MB)) MB"
  $lines.Add($summary)
  $lines.Add("set aside by class: $(($aside.Keys | Sort-Object | ForEach-Object { "$_ $($aside[$_])" }) -join ', ')")
  foreach ($r in $results) {
    $lines.Add("$($r.State)`t$($r.Rel)$(if ($r.Error) { "`t$($r.Error)" })")
    foreach ($s in ($r.Sites | Where-Object Class -eq 'candidate')) {
      $lines.Add("    $($s.Count) in $($s.Register) before call $($s.Callee)")
      $lines.Add("        in $($s.Function)")
    }
    foreach ($s in ($r.Sites | Where-Object Class -eq 'read')) { $lines.Add("    read: $($s.Register) before $($s.Callee): $($s.Reason)") }
    if ($Keep -and (Test-Path (Join-Path $work "$($r.N).plain.asm"))) { $lines.Add("    listings kept: $lvl/$($r.N).plain.asm, $lvl/$($r.N).switched.asm") }
  }
  [IO.File]::WriteAllLines((Join-Path $Out "report.$lvl.txt"), $lines)
  [pscustomobject]@{
    tool = 'miscompile-scan'; level = $lvl; switch = $Switch; preset = $Preset; compiler = "$banner"; seconds = $seconds; jobs = $Jobs
    listing_bytes = $bytes; units = $results.Count; set_aside = $aside
    candidates = @($candidates | ForEach-Object { [pscustomobject]@{ unit = $_.Rel; sites = @($_.Sites | Where-Object Class -eq 'candidate') } })
    failed = @($failed | ForEach-Object { [pscustomobject]@{ unit = $_.Rel; error = $_.Error } })
    read = @($read | ForEach-Object { [pscustomobject]@{ unit = $_.Rel; sites = @($_.Sites | Where-Object Class -eq 'read') } })
  } | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $Out "report.$lvl.json") -Encoding utf8NoBOM
  if (-not (Get-ChildItem $work -Force | Select-Object -First 1)) { Remove-Item $work -ErrorAction SilentlyContinue }   # only when empty
  Write-Host "  /${lvl}: $summary ($seconds s)" -ForegroundColor $(if ($candidates.Count -or $failed.Count) { 'Yellow' } else { 'Green' })
  foreach ($r in $candidates) { Write-Host "    CANDIDATE $($r.Rel)" -ForegroundColor Yellow }
  foreach ($r in $read) { Write-Host "    read before: $($r.Rel)" }
  foreach ($r in $failed) { Write-Host "    did not compile: $($r.Rel)  $($r.Error)" -ForegroundColor Red }
  if ($failed.Count) { $worst = 2 } elseif ($candidates.Count -and $worst -lt 1) { $worst = 1 }
}
# A line of the read list that matched nothing, for a unit this run scanned: the code moved, or, for
# the canary's line, the compiler lost the fault or the scan its sight of it. Said, not failed.
foreach ($e in ($readList | Where-Object { -not $_.Used -and ($_.Unit -in $units.Rel -or ($_.Unit -eq '*' -and -not $Filter)) })) {
  Write-Host "  read list line $($e.Line) matched nothing in this run: $($e.Unit) $($e.Place) $($e.Callee)" -ForegroundColor Yellow
}
Write-Host "miscompile-scan: reports in $Out"
exit $worst
