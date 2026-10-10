#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/mcp-check.ps1, the MCP conformance client. Runs under CTest as
  `mcp_bridge.mcp_check`, which passes the built engine-mcp as -Mcp.

.DESCRIPTION
  Three cases, each in its own scratch directory under one fresh GUID root that is removed again:

    1. A stand-in server, written here, that breaks two of the rules on purpose — it answers a
       notification, and it accepts an edit with no rationale — must make the check fail naming
       both, and the check must still print its table. A conformance client that passes a server
       which breaks the rules it states is worse than none.
    2. An engine-mcp that is not there: exit 2, and nothing else attempted.
    3. The real engine-mcp: every step passes or skips, only a render step may skip and only
       because the bridge said no GPU can render (or because the capture before it skipped), and
       the report holds a response time, a result size and an input-schema size for every tool the
       loop called, and the size of tools/list.

  Without -Mcp the third case uses build/msvc-debug/bin/engine-mcp when it exists and says it is
  skipped when it does not.

      pwsh tools/mcp-check.Tests.ps1 [-Mcp <engine-mcp>] [-KeepTemp]
#>
[CmdletBinding()]
param([string]$Mcp, [switch]$KeepTemp)

$ErrorActionPreference = 'Stop'

$check = Join-Path $PSScriptRoot 'mcp-check.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$root = Join-Path ([IO.Path]::GetTempPath()) "engine-mcp-check-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $root | Out-Null

function Expect([bool]$condition, [string]$what) {
  $script:checks++
  if ($condition) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failures.Add($what) }
}

function Invoke-Check([string[]]$arguments) {
  $out = (& ([Environment]::ProcessPath) -NoProfile -File $check @arguments *>&1 | Out-String)
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

# ---- 1. a server that breaks the rules --------------------------------------------------------

$standIn = Join-Path $root 'stand-in.ps1'
$server = @'
# A stand-in for engine-mcp that gets two things wrong on purpose: it answers notifications, and it
# accepts an edit whatever its attribution says.
$ErrorActionPreference = 'Stop'
function Reply($id, $result) {
  [Console]::Out.WriteLine((@{ jsonrpc = '2.0'; id = $id; result = $result } | ConvertTo-Json -Compress -Depth 20))
  [Console]::Out.Flush()
}
function Result($data, [bool]$isError = $false, [string]$text = 'done') {
  return @{ content = @(@{ type = 'text'; text = $text }); isError = $isError; structuredContent = $data }
}
function Tool([string]$name, [bool]$readOnly) {
  return @{ name = $name; description = "The stand-in's $name."
            inputSchema = @{ type = 'object'; properties = @{ session = @{ type = 'string' } } }
            annotations = @{ readOnlyHint = $readOnly } }
}
$tools = @((Tool 'open_session' $false), (Tool 'create_object' $false), (Tool 'journal' $true))
while ($null -ne ($line = [Console]::In.ReadLine())) {
  $m = $line | ConvertFrom-Json
  $hasId = $null -ne $m.PSObject.Properties['id']
  switch ($m.method) {
    'initialize' {
      Reply $m.id @{ protocolVersion = $m.params.protocolVersion; capabilities = @{ tools = @{ listChanged = $false } }
                     serverInfo = @{ name = 'stand-in'; version = '0' }
                     instructions = 'A stand-in for engine-mcp that accepts every edit and answers notifications.' }
    }
    'ping' { Reply $m.id @{} }
    'tools/list' { Reply $m.id @{ tools = $tools } }
    'tools/call' {
      switch ($m.params.name) {
        'open_session' { Reply $m.id (Result @{ session = 's1' }) }
        'create_object' { Reply $m.id (Result @{ id = ('0' * 31) + '1' }) }
        'journal' { Reply $m.id (Result @{ total = 0; patches = @() }) }
        default { Reply $m.id (Result @{} $true "the stand-in has no $($m.params.name)") }
      }
    }
    default { if (-not $hasId) { Reply $null @{} } }
  }
}
'@
[IO.File]::WriteAllText($standIn, $server, (New-Object System.Text.UTF8Encoding $false))

$r = Invoke-Check @('-Mcp', $standIn, '-Scratch', (Join-Path $root 'stand-in'), '-NoRender')
Expect ($r.Code -eq 1) "a server that breaks the rules fails the check (exit $($r.Code))"
Expect ($r.Out -match 'FAIL\s+notifications/initialized gets no answer[^\n]*a notification was answered') 'the answered notification is named'
Expect ($r.Out -match 'FAIL\s+an edit with no rationale is refused[^\n]*was accepted') 'the edit accepted without a rationale is named'
Expect ($r.Out -match 'Schema B') 'the table is printed after failures'
Expect ($r.Out -match 'ok\s+open_session creates a document') 'a step the stand-in gets right still passes'

# ---- 2. no server -----------------------------------------------------------------------------

$r = Invoke-Check @('-Mcp', (Join-Path $root 'no-such-engine-mcp.exe'), '-Scratch', (Join-Path $root 'none'))
Expect ($r.Code -eq 2) "an engine-mcp that is not there exits 2 (exit $($r.Code))"

# ---- 3. the real bridge -----------------------------------------------------------------------

if (-not $Mcp) {
  $exe = if ($IsWindows) { 'engine-mcp.exe' } else { 'engine-mcp' }
  $Mcp = Join-Path (Split-Path -Parent $PSScriptRoot) "build/msvc-debug/bin/$exe"
}
if (Test-Path -LiteralPath $Mcp) {
  $report = Join-Path $root 'live.json'
  $r = Invoke-Check @('-Mcp', $Mcp, '-Scratch', (Join-Path $root 'live'), '-Report', $report)
  Write-Host $r.Out
  Expect ($r.Code -eq 0) "the loop passes against $Mcp (exit $($r.Code))"
  if (Test-Path -LiteralPath $report) {
    $rep = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    foreach ($s in $rep.steps) {
      if ($s.Status -eq 'skip') {
        $why = ($s.Step -match '^(capture|benchmark)') -and
               ($s.Detail -match 'no GPU can render|did not produce Scene')
        Expect $why "only a render step skips, and only for want of a GPU: $($s.Step): $($s.Detail)"
      } else {
        Expect ($s.Status -eq 'ok') "$($s.Step): $($s.Status) $($s.Detail)"
      }
    }
    Expect ($rep.tools_list_bytes -gt 10000) "tools/list measured ($($rep.tools_list_bytes) bytes)"
    foreach ($row in $rep.tools) {
      if ($row.Tool.StartsWith('(')) { continue }
      Expect (($row.'Schema B' -gt 0) -and ($row.'Max result B' -gt 0) -and ($row.'Max ms' -ge 0)) "$($row.Tool) has a schema size, a result size and a time"
    }
  } else {
    Expect $false 'the check wrote its report'
  }
} else {
  Write-Host "skip  the real bridge: no engine-mcp at $Mcp"
}

if ($KeepTemp) { Write-Host "kept $root" } else { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host ''
Write-Host ("mcp-check.Tests: {0} checks, {1} failed" -f $checks, $failures.Count)
if ($failures.Count -gt 0) { exit 1 }
exit 0
