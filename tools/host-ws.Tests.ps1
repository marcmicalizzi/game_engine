#!/usr/bin/env pwsh
<#
.SYNOPSIS
  engine-host --listen driven by an independent WebSocket client: .NET's
  System.Net.WebSockets.ClientWebSocket. Runs under CTest as `engine_host.host_ws`, which passes the
  built engine-host as -HostExe.

.DESCRIPTION
  apps/engine_host/tests/transport_tests.cpp drives the host through foundation/net's own client.
  A framing bug that the in-tree server and the in-tree client shared — a length encoded and
  decoded the same wrong way, a mask applied twice — would pass every one of those tests. This
  script is the second opinion: a client nobody here wrote, against the same executable.

    1. A connection without the token completes the handshake and is closed with 1008
       (PolicyViolation), and the reason names the token.
    2. With the token: engine.ping answers; a 200 KB doc.apply sent as fifty-one fragments is one
       request and commits; doc.objects answers with that 200 KB value back in one message (the
       server's 64-bit length, read by .NET); doc.journal carries the rationale and the actor.
    3. Two connections at once are both answered.
    4. The close handshake: .NET's CloseAsync gets the server's echo (NormalClosure).
    5. GET / is the shell's index page, and GET /capture/... without the token is a 403.
    6. Closing the host's standard input ends it with exit code 0.

  Without -HostExe it uses build/msvc-debug/bin/engine-host when that exists, and says it skipped
  when it does not. Scratch space is one fresh directory under the system temp directory, removed
  again unless -KeepTemp.

      pwsh tools/host-ws.Tests.ps1 [-HostExe <engine-host>] [-KeepTemp]
#>
[CmdletBinding()]
param([string]$HostExe, [switch]$KeepTemp)

$ErrorActionPreference = 'Stop'

if (-not $HostExe) {
  $suffix = if ($IsWindows -or ($env:OS -eq 'Windows_NT')) { '.exe' } else { '' }
  $HostExe = Join-Path $PSScriptRoot "../build/msvc-debug/bin/engine-host$suffix"
  if (-not (Test-Path -LiteralPath $HostExe)) {
    Write-Host "host-ws: no engine-host at $HostExe; skipped (pass -HostExe)"
    exit 0
  }
}
# ProcessStartInfo resolves a relative path against nothing useful; make it absolute here.
$HostExe = (Resolve-Path -LiteralPath $HostExe).Path

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Expect([bool]$condition, [string]$what) {
  $script:checks++
  if ($condition) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failures.Add($what) }
}

$root = Join-Path ([IO.Path]::GetTempPath()) "engine-host-ws-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $root | Out-Null
$timeout = [TimeSpan]::FromSeconds(60)

function New-Token { [Threading.CancellationTokenSource]::new($timeout).Token }

function Connect-Ws([int]$port, [string]$query) {
  $ws = [System.Net.WebSockets.ClientWebSocket]::new()
  $ws.Options.KeepAliveInterval = [TimeSpan]::Zero
  # A Task is a Task<VoidTaskResult> underneath, and PowerShell would emit the result: discard it.
  $null = $ws.ConnectAsync([Uri]"ws://127.0.0.1:$port/$query", (New-Token)).GetAwaiter().GetResult()
  return $ws
}

# One whole message: text, or the close frame's status when the server closed instead.
function Receive-Message($ws) {
  $buffer = [byte[]]::new(65536)
  $stream = [IO.MemoryStream]::new()
  while ($true) {
    $segment = [ArraySegment[byte]]::new($buffer)
    $r = $ws.ReceiveAsync($segment, (New-Token)).GetAwaiter().GetResult()
    if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
      return [pscustomobject]@{ Close = $true; Status = [int]$r.CloseStatus; Reason = $r.CloseStatusDescription; Text = '' }
    }
    $stream.Write($buffer, 0, $r.Count)
    if ($r.EndOfMessage) { break }
  }
  return [pscustomobject]@{ Close = $false; Status = 0; Reason = ''; Text = [Text.Encoding]::UTF8.GetString($stream.ToArray()) }
}

function Send-Text($ws, [string]$text, [int]$fragment = 0) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($text)
  if ($fragment -le 0) { $fragment = [Math]::Max(1, $bytes.Length) }
  $offset = 0
  $frames = 0
  do {
    $n = [Math]::Min($fragment, $bytes.Length - $offset)
    $last = ($offset + $n) -ge $bytes.Length
    $segment = [ArraySegment[byte]]::new($bytes, $offset, $n)
    $null = $ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $last, (New-Token)).GetAwaiter().GetResult()
    $offset += $n
    $frames++
  } while ($offset -lt $bytes.Length)
  return $frames
}

$script:nextId = 1
function Invoke-Rpc($ws, [string]$method, $params, [int]$fragment = 0) {
  $request = @{ jsonrpc = '2.0'; id = $script:nextId++; method = $method; params = $params } | ConvertTo-Json -Depth 20 -Compress
  $frames = Send-Text $ws $request $fragment
  $m = Receive-Message $ws
  if ($m.Close) { throw "the host closed the connection ($($m.Status) $($m.Reason))" }
  return [pscustomobject]@{ Response = ($m.Text | ConvertFrom-Json -Depth 50); Bytes = $m.Text.Length; Frames = $frames }
}

$hostProcess = $null
try {
  $psi = [System.Diagnostics.ProcessStartInfo]::new($HostExe)
  foreach ($a in @('--listen', '127.0.0.1:0', '--doc', (Join-Path $root 'doc'), '--create', '--actor', 'ps-client')) {
    $psi.ArgumentList.Add($a)
  }
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.UseShellExecute = $false
  $hostProcess = [System.Diagnostics.Process]::Start($psi)
  $line = $hostProcess.StandardOutput.ReadLine()
  $announcement = $line | ConvertFrom-Json
  Expect ($announcement.listen -match '^ws://127\.0\.0\.1:(\d+)/$') "the host announces a loopback ws:// URL ($line)"
  $port = [int]$Matches[1]
  $token = $announcement.token
  $session = $announcement.session
  Expect ($token.Length -eq 22) 'the token is 128 bits of base64url'
  Expect ([bool]$session) 'the announcement names the --doc session'

  # 1. No token.
  $ws = Connect-Ws $port ''
  $m = Receive-Message $ws
  Expect ($m.Close -and $m.Status -eq 1008) "a connection without the token is closed with 1008 (got $($m.Status))"
  Expect ($m.Reason -match 'token') "and the reason names the token ('$($m.Reason)')"
  $ws.Dispose()

  # 2. With the token.
  $ws = Connect-Ws $port "?token=$token"
  $ping = Invoke-Rpc $ws 'engine.ping' $null
  Expect ($ping.Response.result.pid -gt 0) 'engine.ping answers over .NET''s client'

  $bigName = 'n' * (200 * 1024)
  $apply = Invoke-Rpc $ws 'doc.apply' @{
    session = $session
    commands = @(@{ kind = 'CreateObject'; id = '00000000000000300000000000000001'; type = 'engine.scene.MeshPlacement'
                    value = @{ name = $bigName; mesh = 'cube.glb'; translation = @(0, 0, 0) } })
    attribution = @{ rationale = 'two hundred kilobytes from PowerShell' }
  } 4096
  Expect ($apply.Frames -gt 50) "the request went as $($apply.Frames) fragments"
  Expect ($apply.Response.result.committed -eq $true) "a fragmented 200 KB doc.apply commits ($($apply.Response | ConvertTo-Json -Compress -Depth 5 | Select-Object -First 1))"

  $objects = Invoke-Rpc $ws 'doc.objects' @{ session = $session }
  $name = $objects.Response.result.objects[0].properties.name
  Expect ($objects.Bytes -gt 65536) "doc.objects answers in one message of $($objects.Bytes) bytes (a 64-bit length)"
  Expect ($name -eq $bigName) 'and the 200 KB value comes back intact'

  $journal = Invoke-Rpc $ws 'doc.journal' @{ session = $session }
  $entry = $journal.Response.result.patches[0].attribution
  Expect ($entry.rationale -eq 'two hundred kilobytes from PowerShell') 'the journal holds the rationale'
  Expect ($entry.actor -eq 'ps-client') 'and the host''s --actor'

  # 3. A second connection beside the first.
  $ws2 = Connect-Ws $port "?token=$token"
  $a = Invoke-Rpc $ws2 'engine.ping' $null
  $b = Invoke-Rpc $ws 'engine.ping' $null
  Expect ($a.Response.result.pid -eq $b.Response.result.pid) 'two connections are both answered, by the same host'

  # 4. The close handshake.
  $null = $ws2.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'done', (New-Token)).GetAwaiter().GetResult()
  Expect ($ws2.State -eq [System.Net.WebSockets.WebSocketState]::Closed) "CloseAsync completes ($($ws2.State))"
  Expect ($ws2.CloseStatus -eq [System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure) "with the server's echo ($($ws2.CloseStatus))"
  $ws2.Dispose()
  $null = $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'done', (New-Token)).GetAwaiter().GetResult()
  $ws.Dispose()

  # 5. HTTP.
  $http = [System.Net.Http.HttpClient]::new()
  $http.Timeout = $timeout
  $index = $http.GetAsync("http://127.0.0.1:$port/").GetAwaiter().GetResult()
  $body = $index.Content.ReadAsStringAsync().GetAwaiter().GetResult()
  Expect ([int]$index.StatusCode -eq 200 -and $body -match '<html') "GET / is the shell's page ($([int]$index.StatusCode))"
  Expect ($index.Content.Headers.ContentType.MediaType -eq 'text/html') 'served as text/html'
  $capture = $http.GetAsync("http://127.0.0.1:$port/capture/c1").GetAwaiter().GetResult()
  Expect ([int]$capture.StatusCode -eq 403) "a capture without the token is a 403 ($([int]$capture.StatusCode))"
  $http.Dispose()

  # 6. Standard input closed ends the host.
  $hostProcess.StandardInput.Close()
  $exited = $hostProcess.WaitForExit(30000)
  Expect $exited 'closing standard input ends the host'
  if ($exited) { Expect ($hostProcess.ExitCode -eq 0) "with exit code 0 ($($hostProcess.ExitCode))" }
} catch {
  $failures.Add("unexpected: $($_.Exception.Message)")
  Write-Host "FAIL  unexpected: $($_.Exception.Message)"
} finally {
  if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
    # Our own child, started above, which did not end when its input closed: ending it is this
    # test's cleanup, not a stop of anybody else's process.
    try { $hostProcess.StandardInput.Close() } catch { }
    if (-not $hostProcess.WaitForExit(10000)) { $hostProcess.Kill($true) }
  }
  if (-not $KeepTemp) { Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue }
  else { Write-Host "host-ws: kept $root" }
}

Write-Host ("host-ws: {0} checks, {1} failed" -f $checks, $failures.Count)
if ($failures.Count -gt 0) { exit 1 }
exit 0
