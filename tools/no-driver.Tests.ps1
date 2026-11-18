#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/lib/NoDriver.psm1 and `tools/dev.ps1 test -NoDriver`. Runs under CTest as
  `tools.no_driver`.

.DESCRIPTION
  The no-driver form is what a change that needs no GPU is tested with, and a half-formed one
  stands in the machine-wide GPU lock's line for every windowed test (docs/ci/what-to-run.md, "The
  no-driver form"; roadmap T177). So: the four variables are set together and reach a child, they
  are put back as they were (a variable that was absent is absent again), nothing is set without
  the switch, and dev.ps1 uses the one function for both of its no-driver runs. Nothing here runs
  a test, builds, or touches a GPU or the lock.

      pwsh tools/no-driver.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/NoDriver.psm1') -Force

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { Write-Host "       threw: $($_.Exception.Message)"; $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

$names = @('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'ENGINE_GPU_LOCK_ON_DEVICE')
# A build directory that is only a name: nothing is written under it.
$buildDir = Join-Path $PSScriptRoot '../build/no-driver-tests-not-a-preset'
$pwsh = (Get-Process -Id $PID).Path

# What a process this one starts sees of the four: a name maps to its value, or to '<absent>'.
function Get-ChildView {
  $script = '$o = [ordered]@{}; foreach ($n in @(' + (($names | ForEach-Object { "'$_'" }) -join ',') +
            ')) { $v = [Environment]::GetEnvironmentVariable($n); $o[$n] = if ($null -eq $v) { ''<absent>'' } else { $v } }; $o | ConvertTo-Json -Compress'
  $json = & $pwsh -NoProfile -NonInteractive -Command $script
  if ($LASTEXITCODE -ne 0) { throw "the child exited $LASTEXITCODE" }
  return ($json | ConvertFrom-Json -AsHashtable)
}

# This process's own values of the four, put back at the end whatever the cases did.
$original = @{}
foreach ($n in $names) { $original[$n] = [Environment]::GetEnvironmentVariable($n) }
try {
  # A known starting point: two of the four set to something else, two absent.
  [Environment]::SetEnvironmentVariable('VK_DRIVER_FILES', 'C:\somewhere\a-real-driver.json')
  Remove-Item -LiteralPath Env:VK_ICD_FILENAMES -ErrorAction SilentlyContinue
  [Environment]::SetEnvironmentVariable('VK_ADD_DRIVER_FILES', 'C:\somewhere\an-added-driver.json')
  Remove-Item -LiteralPath Env:ENGINE_GPU_LOCK_ON_DEVICE -ErrorAction SilentlyContinue
  $before = Get-ChildView

  $hide = Get-NoDriverEnvironment $buildDir
  Test-That 'the form is four variables: the loader''s three and the lock''s switch' {
    (@($hide.Keys) -join ',') -eq ($names -join ',')
  }
  Test-That 'the driver files name one file under the build directory, which does not exist' {
    $hide.VK_DRIVER_FILES -eq $hide.VK_ICD_FILENAMES -and
    $hide.VK_DRIVER_FILES.StartsWith($buildDir) -and -not (Test-Path -LiteralPath $hide.VK_DRIVER_FILES)
  }
  Test-That 'no driver is added and the switch is 0, the value the test main leaves alone' {
    $null -eq $hide.VK_ADD_DRIVER_FILES -and $hide.ENGINE_GPU_LOCK_ON_DEVICE -eq '0'
  }

  $none = Enter-NoDriver -BuildDir $buildDir -When $false
  Test-That 'without the switch nothing is set and nothing is returned' {
    $null -eq $none -and ((Get-ChildView | ConvertTo-Json -Compress) -eq ($before | ConvertTo-Json -Compress))
  }
  Exit-NoDriver $none
  Test-That 'putting back nothing changes nothing' {
    (Get-ChildView | ConvertTo-Json -Compress) -eq ($before | ConvertTo-Json -Compress)
  }

  $saved = Enter-NoDriver -BuildDir $buildDir
  $inside = Get-ChildView
  Test-That 'with it, a child sees all four: the missing file twice, no added driver, the switch at 0' {
    $inside.VK_DRIVER_FILES -eq $hide.VK_DRIVER_FILES -and $inside.VK_ICD_FILENAMES -eq $hide.VK_ICD_FILENAMES -and
    $inside.VK_ADD_DRIVER_FILES -eq '<absent>' -and $inside.ENGINE_GPU_LOCK_ON_DEVICE -eq '0'
  }

  # dev.ps1's -SkipInvariantGpu pass inside a -NoDriver run enters again and leaves again.
  $inner = Enter-NoDriver -BuildDir $buildDir
  Exit-NoDriver $inner
  Test-That 'a second entry inside the first puts back the first one''s state, not the caller''s' {
    (Get-ChildView | ConvertTo-Json -Compress) -eq ($inside | ConvertTo-Json -Compress)
  }

  Exit-NoDriver $saved
  $after = Get-ChildView
  Test-That 'afterwards each is as it was: values back, and the absent ones absent again' {
    ($after | ConvertTo-Json -Compress) -eq ($before | ConvertTo-Json -Compress) -and
    $after.VK_ICD_FILENAMES -eq '<absent>' -and $after.VK_DRIVER_FILES -eq 'C:\somewhere\a-real-driver.json'
  }

  # dev.ps1 itself: one function for both no-driver runs, the author's put back in a finally.
  $dev = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dev.ps1')
  Test-That 'dev.ps1 test passes its switch to Enter-NoDriver and puts it back in a finally' {
    $dev -match 'Enter-NoDriver -BuildDir \$BuildDir -When \(\[bool\]\$NoDriver\)' -and
    $dev -match 'finally \{ Exit-NoDriver \$noDriverSaved \}'
  }
  Test-That 'the -SkipInvariantGpu pass uses the same function and spells none of the four itself' {
    $dev -match 'Enter-NoDriver -BuildDir \$BuildDir\r?\n' -and -not ($dev -match 'VK_DRIVER_FILES|ENGINE_GPU_LOCK_ON_DEVICE')
  }
  $out = & $pwsh -NoProfile -NonInteractive -File (Join-Path $PSScriptRoot 'dev.ps1') lint -NoDriver 2>&1 | Out-String
  $code = $LASTEXITCODE
  Test-That 'dev.ps1 refuses -NoDriver on anything but a test run, before it does anything' {
    $code -ne 0 -and $out -match "-NoDriver is a test run's switch"
  }
}
finally {
  foreach ($n in $names) { if ($null -eq $original[$n]) { Remove-Item -LiteralPath "Env:$n" -ErrorAction SilentlyContinue } else { [Environment]::SetEnvironmentVariable($n, $original[$n]) } }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
