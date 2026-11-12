# Each CTest pass's LastTest.log, kept under a name of its own (roadmap T88; docs/ci/what-to-run.md,
# "Parts dealt by cost"). CTest writes every invocation's log to the same
# `<build>/Testing/Temporary/LastTest.log`, so the second pass of a `tools/dev.ps1 test`
# (-SkipInvariantGpu's two) and the next invocation (a gate's rerun of one failed test) overwrite
# the first: gate b58's msvc-debug log, the one with a suite's seconds per case, was lost that way.
# tools/dev.ps1's test verb calls Save-CTestLog after every pass; tools/test-costs.Tests.ps1 tests it.

#   Save-CTestLog -BuildDir <build/<preset>> -Pass <name> -Started <time> [-Keep <n>]
# Copies LastTest.log to `LastTest-<yyyyMMdd-HHmmss>-<pass>.log` beside it, the time being the
# pass's start, and returns the copy's path; a second copy in the same second takes `-2`, `-3`. A
# log the pass did not write (older than -Started, or none: ctest stopped before running a test)
# is not copied, and $null comes back. Then only the newest -Keep copies (by name, which is by time)
# are kept, so a build directory does not fill with a day of logs; a log of any other name — a
# gate's `LastTest-linux-server.log` copied in by hand — is never touched.
function Save-CTestLog {
  param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$Pass,
    [Parameter(Mandatory)][datetime]$Started,
    [int]$Keep = 16
  )
  $dir = Join-Path $BuildDir 'Testing/Temporary'
  $log = Join-Path $dir 'LastTest.log'
  if (-not (Test-Path -LiteralPath $log)) { return $null }
  # A second's slack: the file system's time and the clock read before ctest started can differ.
  if ((Get-Item -LiteralPath $log).LastWriteTime -lt $Started.AddSeconds(-1)) { return $null }
  $safe = ($Pass -replace '[^A-Za-z0-9_.-]', '-')
  $stem = 'LastTest-{0}-{1}' -f $Started.ToString('yyyyMMdd-HHmmss'), $safe
  $target = Join-Path $dir "$stem.log"
  $n = 2
  while (Test-Path -LiteralPath $target) { $target = Join-Path $dir "$stem-$n.log"; $n++ }
  Copy-Item -LiteralPath $log -Destination $target
  $copies = @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -match '^LastTest-\d{8}-\d{6}-.+\.log$' } |
      Sort-Object Name -Descending)
  foreach ($old in ($copies | Select-Object -Skip ([math]::Max(1, $Keep)))) {
    Remove-Item -LiteralPath $old.FullName -Force -ErrorAction SilentlyContinue
  }
  return $target
}

Export-ModuleMember -Function Save-CTestLog
