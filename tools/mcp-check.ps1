#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The MCP conformance client: drives engine-mcp from start to finish as an agent's client does,
  runs plan 06 §6.10's one-agent loop as a script of tool calls, and prints what each tool cost.

.DESCRIPTION
  Starts `engine-mcp --workspace <scratch>/workspace --actor mcp-check` and speaks the Model Context
  Protocol to it over stdio exactly as Claude Code or any other client does: newline-delimited
  JSON-RPC 2.0, `initialize` with a protocol version, `notifications/initialized`, `tools/list`,
  then `tools/call` requests with increasing ids. Nothing in this script knows the bridge's C++;
  everything it asserts is on the wire.

  The loop (docs/plan/06-agent-tooling.md §6.10: checkpoint, edit, build, validate, capture,
  review, accept or roll back): open (create) a session, find a type with list_schema and read its
  schema with describe, create an object and set a property — each with a rationale — read the
  journal, add a layer and read the diff, validate, capture a frame, benchmark a short camera path,
  read the log tail, undo, close. Around it, the invariants the plan states:

    - every edit is one undoable transaction carrying its rationale and the bridge's actor
      (one journal patch per call, one forward command each);
    - an edit with no rationale is refused before it reaches the host, and journals nothing;
    - a notification gets no answer;
    - under a role that may not write (content/roles/roles.json's `qa`) the read-only tools are
      offered and succeed, the writing tools are not offered and a call to one is refused, and a
      read-only tool is never withheld; under `designer` (review required) a direct edit is
      refused by the host with error 1008 and a hint to propose instead.

  The render steps skip, with the reason and without failing, where the bridge says no GPU can
  render (error 1007), so the check runs on a CI runner too.

  It records, per tool, the response time and the byte size of the tool's `inputSchema` (as the
  bridge wrote it, measured on the raw `tools/list` text) and of its result, and prints a table and
  the size of the whole `tools/list` (what enters an agent's context on every session).

  It is a test, so it owns nothing outside its scratch directory: `-Scratch` names one (which it
  creates and leaves), and without it a fresh directory under the system temp directory is made and
  removed again unless a step failed or -KeepScratch is given.

  Exit codes: 0 every step passed or skipped, 1 a step failed, 2 engine-mcp could not be started.

  -Connect runs the same loop with the bridge attached to a running host (roadmap A6): it starts
  `engine-host --listen 127.0.0.1:0 --doc <scratch>/world --create` (the engine-host beside
  engine-mcp), saves the host's first stdout line and starts `engine-mcp --connect-from` it. A
  second, plain WebSocket client (.NET's ClientWebSocket, nothing of the bridge's) subscribes to
  the session's journal before the loop and, after the edits, must have been pushed an
  `event.journal` for each of the bridge's commits and must read the bridge's entries — actor,
  rationale — in `doc.journal` over its own connection: the agent and the owner are on one
  session of one host. The bridge's MCP client subscribes to the journal resource and must be sent
  `notifications/resources/updated` for it. The roles steps skip: an attached bridge is governed by
  the host's roles, and the spawned run covers them.

      pwsh tools/mcp-check.ps1 [-Mcp build/msvc-debug/bin/engine-mcp.exe] [-Scratch <dir>]
                               [-Report <file.json>] [-NoRender] [-KeepScratch] [-Connect]

  docs/subsystems/apps.md ("The conformance client") has what the first run found.
#>
[CmdletBinding()]
param(
  # The engine-mcp to drive. A .ps1 is run with this PowerShell (mcp-check.Tests.ps1's stand-in).
  [string]$Mcp,
  [string]$Scratch,
  [string]$Roles,
  [string]$ProtocolVersion = '2025-11-25',
  # Also write every step and measurement here, as JSON.
  [string]$Report,
  # Seconds a quick call may take, and a render call (which can wait half an hour for the GPU lock,
  # ADR-0049, before it starts).
  [double]$CallTimeout = 300,
  [double]$RenderTimeout = 2700,
  [switch]$NoRender,
  [switch]$KeepScratch,
  # Attach the bridge to a running engine-host --listen instead of letting it start one.
  [switch]$Connect
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Mcp) {
  $exe = if ($IsWindows) { 'engine-mcp.exe' } else { 'engine-mcp' }
  $Mcp = Join-Path $repo "build/msvc-debug/bin/$exe"
}
if (-not (Test-Path -LiteralPath $Mcp)) {
  Write-Host "mcp-check: no engine-mcp at $Mcp (build a preset, or pass -Mcp)"
  exit 2
}
$Mcp = (Resolve-Path -LiteralPath $Mcp).Path
if (-not $Roles) { $Roles = Join-Path $repo 'content/roles/roles.json' }

$ownScratch = -not $Scratch
if ($ownScratch) {
  # engine-lint: allow-temp-path a fresh GUID per run, removed afterwards: two copies never meet
  $Scratch = Join-Path ([IO.Path]::GetTempPath()) "engine-mcp-check-$([guid]::NewGuid().ToString('N'))"
}
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
$Scratch = (Resolve-Path -LiteralPath $Scratch).Path

$utf8 = [Text.UTF8Encoding]::new($false)
$steps = [Collections.Generic.List[object]]::new()
$calls = [Collections.Generic.List[object]]::new()
$ctx = @{}
# The object type the loop edits: a document type every preset has (schemas/content.schema).
$objectType = 'engine.content.AssetProvenance'

# ---- the wire ---------------------------------------------------------------------------------

function Start-Mcp([string]$label, [string[]]$extra) {
  $psi = [Diagnostics.ProcessStartInfo]::new()
  if ($Mcp -like '*.ps1') {
    $psi.FileName = [Environment]::ProcessPath
    foreach ($a in @('-NoProfile', '-File', $Mcp)) { $psi.ArgumentList.Add($a) }
  } else {
    $psi.FileName = $Mcp
  }
  $ws = Join-Path $Scratch 'workspace'
  foreach ($a in @('--workspace', $ws, '--actor', 'mcp-check') + @($extra | Where-Object { $_ })) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.StandardInputEncoding = $utf8
  $psi.StandardOutputEncoding = $utf8
  $psi.StandardErrorEncoding = $utf8
  $psi.WorkingDirectory = $Scratch
  $p = [Diagnostics.Process]::Start($psi)
  # MCP's stdio transport is newline-delimited; a client on Windows still sends a bare LF.
  $p.StandardInput.NewLine = "`n"
  $p.StandardInput.AutoFlush = $true
  # stderr is the server's log: drained as it comes so a chatty host never blocks on a full pipe.
  return @{ Label = $label; Process = $p; Err = $p.StandardError.ReadToEndAsync(); NextId = 1
            Stray = [Collections.Generic.List[string]]::new() }
}

function Stop-Mcp($server) {
  if ($null -eq $server) { return }
  $p = $server.Process
  try { $p.StandardInput.Close() } catch { }
  # A bridge whose input ends stops its host and exits; it is given a while to do so.
  [void]$p.WaitForExit(60000)
  $code = if ($p.HasExited) { $p.ExitCode } else { $null }
  $log = if ($server.Err.Wait(5000)) { $server.Err.Result } else { '' }
  [IO.File]::WriteAllText((Join-Path $Scratch "$($server.Label).stderr.log"), $log, $utf8)
  return $code
}

function Read-McpLine($server, [double]$seconds) {
  $task = $server.Process.StandardOutput.ReadLineAsync()
  if (-not $task.Wait([int]($seconds * 1000))) {
    throw "engine-mcp wrote nothing for $seconds s"
  }
  return $task.Result
}

function Send-Rpc($server, [string]$method, $params, [switch]$Notification, [double]$Timeout = $CallTimeout) {
  $message = [ordered]@{ jsonrpc = '2.0' }
  $id = $null
  if (-not $Notification) { $id = $server.NextId; $server.NextId++; $message.id = $id }
  $message.method = $method
  if ($null -ne $params) { $message.params = $params }
  $line = $message | ConvertTo-Json -Compress -Depth 64
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $server.Process.StandardInput.WriteLine($line)
  # Every line both ways, for a reader who wants to see what an agent would have seen.
  [IO.File]::AppendAllText((Join-Path $Scratch 'transcript.jsonl'), "> $($server.Label) $line`n", $utf8)
  if ($Notification) { return }
  while ($true) {
    $text = Read-McpLine $server $Timeout
    if ($null -eq $text) { throw "engine-mcp closed its output before answering $method (id $id)" }
    [IO.File]::AppendAllText((Join-Path $Scratch 'transcript.jsonl'), "< $($server.Label) $text`n", $utf8)
    $answer = $text | ConvertFrom-Json -Depth 128
    if ((Get-Prop $answer 'id') -eq $id) { break }
    # A line that answers nothing we asked: a reply to a notification, or a server-sent request.
    $server.Stray.Add($text)
  }
  $clock.Stop()
  return [pscustomobject]@{ Answer = $answer; Text = $text; Ms = $clock.Elapsed.TotalMilliseconds
                            Bytes = $utf8.GetByteCount($text) }
}

# One tools/call, timed and sized. `Ok`: a result that is not an error.
function Invoke-Tool($server, [string]$name, $arguments, [double]$Timeout = $CallTimeout) {
  if ($null -eq $arguments) { $arguments = @{} }
  $r = Send-Rpc $server 'tools/call' @{ name = $name; arguments = $arguments } -Timeout $Timeout
  $result = Get-Prop $r.Answer 'result'
  $rpcError = Get-Prop $r.Answer 'error'
  $text = ''
  foreach ($block in @(Get-Prop $result 'content')) {
    if ((Get-Prop $block 'type') -eq 'text') { $text += [string](Get-Prop $block 'text') + "`n" }
  }
  $isError = ($null -ne $rpcError) -or ((Get-Prop $result 'isError') -eq $true)
  $calls.Add([pscustomobject]@{ Bridge = $server.Label; Tool = $name; Ms = $r.Ms; Bytes = $r.Bytes
                                TextBytes = $utf8.GetByteCount($text); Error = $isError })
  return [pscustomobject]@{
    Ok = -not $isError; RpcError = $rpcError; Result = $result; Text = $text
    Data = (Get-Prop $result 'structuredContent'); Ms = $r.Ms; Bytes = $r.Bytes
  }
}

# ---- reading what came back -------------------------------------------------------------------

# A member path ('a.b.c') of a parsed JSON value, or nothing when any part is absent. An array comes
# back through the pipeline, so a caller that wants one writes @(Get-Prop ...).
function Get-Prop($value, [string]$path) {
  $cur = $value
  foreach ($part in $path -split '\.') {
    if ($null -eq $cur) { return }
    if ($cur -is [Collections.IDictionary]) {
      if (-not $cur.Contains($part)) { return }
      $cur = $cur[$part]
      continue
    }
    $member = $cur.PSObject.Properties[$part]
    if ($null -eq $member) { return }
    $cur = $member.Value
  }
  return $cur
}

function Assert([bool]$condition, [string]$message) {
  if (-not $condition) { throw $message }
}

function Skip([string]$reason) { throw "SKIP: $reason" }

# The text a failed tool call showed, one line, for a step's detail.
function Show($call) {
  $t = if ($call.RpcError) { "JSON-RPC $($call.RpcError.code): $($call.RpcError.message)" } else { $call.Text }
  $t = ($t -replace '\s+', ' ').Trim()
  if ($t.Length -gt 300) { $t = $t.Substring(0, 300) + '...' }
  return $t
}

function Assert-Ok($call, [string]$what) {
  Assert $call.Ok "$what failed: $(Show $call)"
}

function Step([string]$name, [scriptblock]$body) {
  $status = 'ok'
  $detail = ''
  try {
    $out = & $body
    if ($null -ne $out) { $detail = [string](@($out)[-1]) }
  } catch {
    $msg = $_.Exception.Message
    if ($msg.StartsWith('SKIP: ')) { $status = 'skip'; $detail = $msg.Substring(6) }
    else { $status = 'FAIL'; $detail = $msg }
  }
  $steps.Add([pscustomobject]@{ Step = $name; Status = $status; Detail = $detail })
  $tag = switch ($status) { 'ok' { 'ok  ' } 'skip' { 'skip' } default { 'FAIL' } }
  Write-Host ("{0}  {1}{2}" -f $tag, $name, $(if ($detail) { ": $detail" } else { '' }))
}

function Need([string]$key) {
  if (-not $ctx.ContainsKey($key) -or $null -eq $ctx[$key]) { Skip "an earlier step did not produce $key" }
  return $ctx[$key]
}

function Journal-Total($server, [string]$session) {
  $j = Invoke-Tool $server 'journal' @{ session = $session }
  Assert-Ok $j 'journal'
  return [int](Get-Prop $j.Data 'total')
}

# A render tool's answer: the result, or a skip when the bridge says no GPU here can render.
function Render-Result($call, [string]$what) {
  $code = Get-Prop $call.Data 'error.code'
  if (-not $call.Ok -and $code -eq 1007) { Skip "no GPU can render here: $(Show $call)" }
  Assert-Ok $call $what
}

function Rationale([string]$why) { return @{ rationale = $why } }

# ---- -Connect: the running host, and a second client on it ----------------------------------

$hostProcess = $null
$watch = $null
function New-WsToken { [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(120)).Token }

# One whole text message from the plain client, or $null when the connection closed.
function Receive-Ws($ws) {
  $buffer = [byte[]]::new(65536)
  $stream = [IO.MemoryStream]::new()
  while ($true) {
    $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buffer), (New-WsToken)).GetAwaiter().GetResult()
    if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { return $null }
    $stream.Write($buffer, 0, $r.Count)
    if ($r.EndOfMessage) { break }
  }
  return $utf8.GetString($stream.ToArray())
}

# A request on the plain client; the events pushed meanwhile are kept, with when they came.
function Invoke-Ws($method, $params) {
  $id = $watch.NextId; $watch.NextId++
  $line = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 32
  $bytes = $utf8.GetBytes($line)
  $null = $watch.Ws.SendAsync([ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, (New-WsToken)).GetAwaiter().GetResult()
  while ($true) {
    $text = Receive-Ws $watch.Ws
    if ($null -eq $text) { throw "the host closed the plain client's connection" }
    $m = $text | ConvertFrom-Json -Depth 64
    if ($null -eq (Get-Prop $m 'id')) { $watch.Events.Add([pscustomobject]@{ At = $watch.Clock.Elapsed.TotalMilliseconds; Message = $m }); continue }
    return $m
  }
}

if ($Connect) {
  $hostExe = Join-Path (Split-Path -Parent $Mcp) $(if ($IsWindows) { 'engine-host.exe' } else { 'engine-host' })
  if (-not (Test-Path -LiteralPath $hostExe)) {
    Write-Host "mcp-check: -Connect needs engine-host beside engine-mcp ($hostExe)"
    exit 2
  }
  $psi = [Diagnostics.ProcessStartInfo]::new($hostExe)
  foreach ($a in @('--listen', '127.0.0.1:0', '--doc', (Join-Path $Scratch 'world'), '--create', '--actor', 'owner')) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $hostProcess = [Diagnostics.Process]::Start($psi)
  $hostErr = $hostProcess.StandardError.ReadToEndAsync()
  $announcementLine = $hostProcess.StandardOutput.ReadLine()
  $announcementFile = Join-Path $Scratch 'host.json'
  [IO.File]::WriteAllText($announcementFile, "$announcementLine`n", $utf8)
  $announcement = $announcementLine | ConvertFrom-Json
  $null = $announcement.listen -match ':(\d+)/$'
  $watch = @{ Ws = [System.Net.WebSockets.ClientWebSocket]::new(); NextId = 1
              Events = [Collections.Generic.List[object]]::new(); Clock = [Diagnostics.Stopwatch]::StartNew()
              Session = [string]$announcement.session; Port = [int]$Matches[1] }
  $watch.Ws.Options.KeepAliveInterval = [TimeSpan]::Zero
  $watch.Ws.Options.SetRequestHeader('X-Engine-Token', [string]$announcement.token)
  $null = $watch.Ws.ConnectAsync([Uri]"ws://127.0.0.1:$($watch.Port)/", (New-WsToken)).GetAwaiter().GetResult()
  $sub = Invoke-Ws 'session.subscribe' @{ session = $watch.Session; kinds = @('journal') }
  if ($null -eq (Get-Prop $sub 'result')) { Write-Host "mcp-check: the plain client could not subscribe: $($sub | ConvertTo-Json -Compress -Depth 8)"; exit 2 }
  Write-Host "mcp-check: attached to engine-host on 127.0.0.1:$($watch.Port), session $($watch.Session)"
}

# ---- the session ------------------------------------------------------------------------------

$bridge = $null
$exit = 0
try {
  Write-Host "mcp-check: $Mcp"
  Write-Host "mcp-check: scratch $Scratch"
  try {
    $bridge = Start-Mcp 'agent' $(if ($Connect) { @('--connect-from', $announcementFile) } else { @() })
  } catch {
    Write-Host "mcp-check: engine-mcp did not start: $($_.Exception.Message)"
    exit 2
  }

  Step 'initialize: the version asked for, tools, instructions' {
    $r = Send-Rpc $bridge 'initialize' @{
      protocolVersion = $ProtocolVersion
      capabilities = @{}
      clientInfo = @{ name = 'mcp-check'; version = '1' }
    } -Timeout 600
    $ctx.InitMs = $r.Ms
    $calls.Add([pscustomobject]@{ Bridge = 'agent'; Tool = '(initialize)'; Ms = $r.Ms; Bytes = $r.Bytes
                                  TextBytes = 0; Error = $false })
    $res = Get-Prop $r.Answer 'result'
    Assert ($null -ne $res) "initialize answered no result: $($r.Text)"
    $ctx.Version = [string](Get-Prop $res 'protocolVersion')
    Assert ($ctx.Version -eq $ProtocolVersion) "asked for $ProtocolVersion, agreed $($ctx.Version)"
    Assert ($null -ne (Get-Prop $res 'capabilities.tools')) 'no tools capability'
    Assert ([bool][string](Get-Prop $res 'serverInfo.name')) 'no serverInfo.name'
    $instructions = [string](Get-Prop $res 'instructions')
    Assert ($instructions.Length -gt 40) 'no instructions for the model'
    "agreed $($ctx.Version), $($instructions.Length) characters of instructions, $([int]$r.Ms) ms (host start included)"
  }

  Step 'notifications/initialized gets no answer; ping does' {
    Send-Rpc $bridge 'notifications/initialized' $null -Notification
    $r = Send-Rpc $bridge 'ping' $null
    Assert ($bridge.Stray.Count -eq 0) "a notification was answered: $(@($bridge.Stray) -join ' ')"
    $res = Get-Prop $r.Answer 'result'
    Assert ($null -ne $res) "ping answered $($r.Text)"
  }

  Step 'tools/list: every tool named, described, an object schema' {
    $r = Send-Rpc $bridge 'tools/list' @{}
    $ctx.ListBytes = $r.Bytes
    $ctx.ListMs = $r.Ms
    $calls.Add([pscustomobject]@{ Bridge = 'agent'; Tool = '(tools/list)'; Ms = $r.Ms; Bytes = $r.Bytes
                                  TextBytes = 0; Error = $false })
    $tools = @(Get-Prop $r.Answer 'result.tools')
    Assert ($tools.Count -gt 0) 'no tools'
    Assert ($null -eq (Get-Prop $r.Answer 'result.nextCursor')) 'tools/list paged; this check reads one page'
    # The schema sizes come from the raw text, as the bridge wrote it, not from a re-serialization.
    $raw = [Text.Json.JsonDocument]::Parse($r.Text)
    $sizes = @{}
    $descriptions = @{}
    foreach ($el in $raw.RootElement.GetProperty('result').GetProperty('tools').EnumerateArray()) {
      $n = $el.GetProperty('name').GetString()
      $sizes[$n] = $utf8.GetByteCount($el.GetProperty('inputSchema').GetRawText())
      $descriptions[$n] = $utf8.GetByteCount($el.GetRawText())
    }
    $raw.Dispose()
    $ctx.SchemaBytes = $sizes
    $ctx.ToolBytes = $descriptions
    $ctx.Tools = @{}
    $problems = [Collections.Generic.List[string]]::new()
    foreach ($t in $tools) {
      $n = [string](Get-Prop $t 'name')
      $ctx.Tools[$n] = $t
      if (-not [string](Get-Prop $t 'description')) { $problems.Add("$n has no description") }
      $schema = Get-Prop $t 'inputSchema'
      if ((Get-Prop $schema 'type') -ne 'object') { $problems.Add("$n's inputSchema is not an object"); continue }
      $props = Get-Prop $schema 'properties'
      foreach ($req in @(Get-Prop $schema 'required')) {
        if ($null -eq $props -or $null -eq $props.PSObject.Properties[[string]$req]) {
          $problems.Add("$n requires '$req', which it does not describe")
        }
      }
      if ($ctx.Version -ge '2025-03-26' -and ($null -eq (Get-Prop $t 'annotations.readOnlyHint'))) {
        $problems.Add("$n has no readOnlyHint")
      }
    }
    Assert ($problems.Count -eq 0) ($problems -join '; ')
    $schemaTotal = ($sizes.Values | Measure-Object -Sum).Sum
    "$($tools.Count) tools, $($r.Bytes) bytes (~$([int]($r.Bytes / 4)) tokens), $schemaTotal bytes of it input schemas"
  }

  Step 'host_info names the actor and the workspace' {
    $c = Invoke-Tool $bridge 'host_info' @{}
    Assert-Ok $c 'host_info'
    Assert ($c.Text -match 'mcp-check') "host_info does not name the actor mcp-check: $(Show $c)"
    $ctx.HostPid = Get-Prop $c.Data 'host.pid'
    "host pid $($ctx.HostPid)"
  }

  Step 'open_session creates a document (the checkpoint: journal position 0)' {
    $doc = Join-Path $Scratch 'world'
    $c = Invoke-Tool $bridge 'open_session' @{ path = $doc; create = $true; name = 'mcp-check' }
    Assert-Ok $c 'open_session'
    $s = [string](Get-Prop $c.Data 'session')
    Assert ([bool]$s) "open_session returned no session: $(Show $c)"
    $ctx.Session = $s
    $ctx.Doc = $doc
    Assert ((Journal-Total $bridge $s) -eq 0) 'a new document has a journal'
    if ($Connect) {
      # The host's own document, joined rather than opened again.
      Assert ($s -eq $watch.Session) "open_session made $s beside the host's $($watch.Session)"
      $r = Send-Rpc $bridge 'resources/subscribe' @{ uri = "engine://session/$s/journal" }
      Assert ($null -ne (Get-Prop $r.Answer 'result')) "resources/subscribe: $($r.Text)"
    }
    "session $s"
  }

  Step 'list_schema finds an object type; describe reads its schema' {
    $s = Need 'Session'
    $c = Invoke-Tool $bridge 'list_schema' @{ namespace = 'engine.content' }
    Assert-Ok $c 'list_schema'
    Assert ($c.Text -match 'engine\.content\.AssetProvenance') "list_schema engine.content does not list AssetProvenance: $(Show $c)"
    $d = Invoke-Tool $bridge 'describe' @{ type = 'engine.content.AssetProvenance' }
    Assert-Ok $d 'describe'
    Assert ($d.Text -match 'generator') "describe AssetProvenance names no generator field: $(Show $d)"
    "$($d.Bytes) bytes for one type"
  }

  Step 'an edit with no rationale is refused and journals nothing' {
    $s = Need 'Session'
    $t = $objectType
    $bare = Invoke-Tool $bridge 'create_object' @{ session = $s; type = $t }
    Assert (-not $bare.Ok) 'create_object with no attribution was accepted'
    Assert ($bare.Text -match 'attribution') "the refusal does not name attribution: $(Show $bare)"
    $blank = Invoke-Tool $bridge 'create_object' @{ session = $s; type = $t; attribution = @{ rationale = '  ' } }
    Assert (-not $blank.Ok) 'create_object with a blank rationale was accepted'
    Assert ($blank.Text -match 'rationale') "the refusal does not name the rationale: $(Show $blank)"
    Assert ($blank.Text -match 'Hint:') "the refusal gives no hint: $(Show $blank)"
    Assert ((Journal-Total $bridge $s) -eq 0) 'a refused edit reached the journal'
  }

  Step 'create_object and set_property: one transaction each, with the rationale' {
    $s = Need 'Session'
    $t = $objectType
    $c = Invoke-Tool $bridge 'create_object' @{
      session = $s; type = $t; properties = @{ generator = 'mcp-check' }
      attribution = (Rationale 'the conformance client needs an object to edit')
    }
    Assert-Ok $c 'create_object'
    $id = [string](Get-Prop $c.Data 'id')
    Assert ($id -match '^[0-9a-f]{32}$') "create_object returned no generated id: $(Show $c)"
    $ctx.Object = $id
    Assert ((Journal-Total $bridge $s) -eq 1) 'create_object did not journal exactly one transaction'
    $p = Invoke-Tool $bridge 'set_property' @{
      session = $s; id = $id; name = 'generator'; value = 'mcp-check-2'
      attribution = (Rationale 'a property change to undo later')
    }
    Assert-Ok $p 'set_property'
    Assert ((Journal-Total $bridge $s) -eq 2) 'set_property did not journal exactly one transaction'
    "object $id"
  }

  if ($Connect) {
    Step 'the shared session: a plain client on the host was pushed each commit and reads them' {
      $s = Need 'Session'
      # One more edit through the bridge, timed from the tools/call to the plain client's event.
      $id = Need 'Object'
      $sent = $watch.Clock.Elapsed.TotalMilliseconds
      $before = $watch.Events.Count
      $receive = $watch.Ws.ReceiveAsync([ArraySegment[byte]]::new([byte[]]::new(65536)), (New-WsToken))
      $e = Invoke-Tool $bridge 'set_property' @{ session = $s; id = $id; name = 'generator'; value = 'shared'; attribution = (Rationale 'the owner should see this arrive') }
      Assert-Ok $e 'set_property through the attached bridge'
      $null = $receive.GetAwaiter().GetResult()
      $ctx.EventMs = $watch.Clock.Elapsed.TotalMilliseconds - $sent
      # What the shell does on that event: read the objects again, over its own connection.
      $t0 = $watch.Clock.Elapsed.TotalMilliseconds
      $objects = Invoke-Ws 'doc.objects' @{ session = $s }
      $ctx.OutlinerMs = $watch.Clock.Elapsed.TotalMilliseconds - $t0
      Assert ($null -ne (Get-Prop $objects 'result')) 'doc.objects over the plain client'
      $journal = Invoke-Ws 'doc.journal' @{ session = $s }
      $patches = @(Get-Prop $journal 'result.patches')
      Assert ($patches.Count -ge 3) "the plain client reads $($patches.Count) patches"
      $mine = @($patches | Where-Object { (Get-Prop $_ 'attribution.actor') -eq 'mcp-check' })
      Assert ($mine.Count -eq $patches.Count) 'a patch on the shared session is not the bridge''s'
      Assert ((Get-Prop $patches[-1] 'attribution.rationale') -eq 'the owner should see this arrive') 'the last rationale'
      # Events: the receive above took one (the last commit's); the rest were kept by Invoke-Ws.
      $pushed = @($watch.Events | Where-Object { (Get-Prop $_.Message 'method') -eq 'event.journal' })
      Assert (($pushed.Count + 1) -ge $patches.Count) "pushed $($pushed.Count + 1) journal events for $($patches.Count) commits"
      # And the bridge's own MCP client, which subscribed to the journal resource.
      $updated = @($bridge.Stray | Where-Object { $_ -match 'notifications/resources/updated' -and $_ -match "engine://session/$s/journal" })
      Assert ($updated.Count -ge 1) "the MCP client was sent no notifications/resources/updated (stray: $(@($bridge.Stray) -join ' | '))"
      "{0} patches by the bridge read by a second client; event {1:N1} ms after the tools/call, doc.objects {2:N1} ms; {3} resources/updated to the MCP client" -f $patches.Count, $ctx.EventMs, $ctx.OutlinerMs, $updated.Count
    }
  }

  Step 'journal: every patch carries the actor and its rationale' {
    $s = Need 'Session'
    $j = Invoke-Tool $bridge 'journal' @{ session = $s }
    Assert-Ok $j 'journal'
    $patches = @(Get-Prop $j.Data 'patches')
    # Attached, the shared-session step above made a third.
    $expected = if ($Connect) { 3 } else { 2 }
    Assert ($patches.Count -eq $expected) "expected $expected patches, read $($patches.Count)"
    $want = @('the conformance client needs an object to edit', 'a property change to undo later')
    for ($i = 0; $i -lt 2; $i++) {
      $a = Get-Prop $patches[$i] 'attribution'
      Assert ((Get-Prop $a 'actor') -eq 'mcp-check') "patch $i actor is '$(Get-Prop $a 'actor')'"
      Assert ((Get-Prop $a 'rationale') -eq $want[$i]) "patch $i rationale is '$(Get-Prop $a 'rationale')'"
      Assert ((Get-Prop $patches[$i] 'forward_commands') -eq 1) "patch $i is not one command"
    }
  }

  Step 'add_layer, an override in it, and the diff' {
    $s = Need 'Session'
    $id = Need 'Object'
    $l = Invoke-Tool $bridge 'add_layer' @{ session = $s; name = 'review' }
    Assert-Ok $l 'add_layer'
    $p = Invoke-Tool $bridge 'set_property' @{
      session = $s; id = $id; name = 'generator'; value = 'reviewed'
      attribution = (Rationale 'an override for the diff to show')
    }
    Assert-Ok $p 'set_property in the review layer'
    $d = Invoke-Tool $bridge 'diff' @{ session = $s; from_layer = 'base'; to_layer = 'review' }
    Assert-Ok $d 'diff'
    $commands = @(Get-Prop $d.Data 'commands')
    Assert ($commands.Count -ge 1) "the diff is empty: $(Show $d)"
    Assert ((Get-Prop $d.Data 'total') -eq $commands.Count) 'the diff total does not count its commands'
    "$($commands.Count) command(s)"
  }

  Step 'validate' {
    $s = Need 'Session'
    $v = Invoke-Tool $bridge 'validate' @{ session = $s }
    Assert-Ok $v 'validate'
    Assert ((Get-Prop $v.Data 'ok') -eq $true) "the document does not validate: $(Show $v)"
  }

  Step 'capture a frame into the workspace' {
    if ($NoRender) { Skip '-NoRender' }
    $c = Invoke-Tool $bridge 'capture' @{ load = @{ grid = 33 }; width = 320; height = 180; channels = @('color', 'ids') } -Timeout $RenderTimeout
    Render-Result $c 'capture'
    Assert ($c.Bytes -lt 8192) "the capture's answer is $($c.Bytes) bytes; bulk data belongs in files"
    $links = @(@(Get-Prop $c.Result 'content') | Where-Object { (Get-Prop $_ 'type') -eq 'resource_link' })
    if ($ctx.Version -ge '2025-06-18') { Assert ($links.Count -ge 1) 'no resource_link for the picture' }
    foreach ($link in $links) {
      $path = ([Uri][string](Get-Prop $link 'uri')).LocalPath
      Assert (Test-Path -LiteralPath $path) "the capture names $path, which is not there"
    }
    $ctx.Scene = [string](Get-Prop $c.Data 'scene')
    $ctx.Capture = $c
    "$($links.Count) file(s), scene $($ctx.Scene), answer $($c.Bytes) bytes"
  }

  Step 'benchmark a short camera path' {
    if ($NoRender) { Skip '-NoRender' }
    $scene = Need 'Scene'
    # A path of its own, in the check's scratch: two keys a fifth of a second apart, at 60 fps.
    $path = Join-Path $Scratch 'short-path.json'
    $camera = Get-Prop $ctx.Capture.Data 'camera'
    $eye = @(Get-Prop $camera 'position')
    $at = @(Get-Prop $camera 'target')
    if ($eye.Count -ne 3 -or $at.Count -ne 3) { $eye = @(0, 60, 120); $at = @(0, 0, 0) }
    $moved = @(($eye[0] + 2), $eye[1], $eye[2])
    $doc = [ordered]@{
      format = 'engine.camera-path.v1'; name = 'mcp-check'; fps = 60; interpolation = 'Smooth'
      keys = @(
        [ordered]@{ time = 0; position = $eye; target = $at; fov_deg = 60 },
        [ordered]@{ time = 0.2; position = $moved; target = $at; fov_deg = 60 }
      )
    }
    [IO.File]::WriteAllText($path, ($doc | ConvertTo-Json -Depth 8), $utf8)
    $b = Invoke-Tool $bridge 'benchmark' @{ scene = $scene; camera_path = $path; width = 320; height = 180 } -Timeout $RenderTimeout
    Render-Result $b 'benchmark'
    $files = @(@(Get-Prop $b.Result 'content') | Where-Object { (Get-Prop $_ 'type') -eq 'resource_link' })
    foreach ($link in $files) {
      $p = ([Uri][string](Get-Prop $link 'uri')).LocalPath
      Assert (Test-Path -LiteralPath $p) "the benchmark names $p, which is not there"
    }
    Assert ($b.Text -match 'frame|ms') "the benchmark says nothing about frames: $(Show $b)"
    "$($files.Count) file(s), $([int]$b.Ms) ms"
  }

  Step 'get_logs: the log tail' {
    $g = Invoke-Tool $bridge 'get_logs' @{}
    Assert-Ok $g 'get_logs'
    Assert ($null -ne (Get-Prop $g.Data 'records')) "get_logs has no records: $(Show $g)"
    Assert ($null -ne (Get-Prop $g.Data 'next')) "get_logs has no next: $(Show $g)"
    "$(@(Get-Prop $g.Data 'records').Count) record(s)"
  }

  Step 'undo rolls the last edit back' {
    $s = Need 'Session'
    $id = Need 'Object'
    $u = Invoke-Tool $bridge 'undo' @{ session = $s }
    Assert-Ok $u 'undo'
    Assert ((Get-Prop $u.Data 'stepped') -eq 1) "undo stepped $(Get-Prop $u.Data 'stepped')"
    $g = Invoke-Tool $bridge 'get' @{ session = $s; id = $id }
    Assert-Ok $g 'get'
    $value = Get-Prop $g.Data 'properties.generator'
    # Attached, the shared-session step's edit is the base layer's latest.
    $base = if ($Connect) { 'shared' } else { 'mcp-check-2' }
    Assert ($value -eq $base) "after undo the property is '$value', not the base layer's '$base'"
  }

  Step 'close_session; the id names nothing afterwards' {
    $s = Need 'Session'
    $c = Invoke-Tool $bridge 'close_session' @{ session = $s }
    Assert-Ok $c 'close_session'
    $l = Invoke-Tool $bridge 'list_sessions' @{}
    Assert-Ok $l 'list_sessions'
    Assert (@(Get-Prop $l.Data 'sessions').Count -eq 0) 'a session is still open'
    $gone = Invoke-Tool $bridge 'journal' @{ session = $s }
    Assert (-not $gone.Ok) 'a closed session still answers'
    Assert ($gone.Text -match 'Hint:') "the closed session's error has no hint: $(Show $gone)"
  }

  $code = Stop-Mcp $bridge
  $bridge = $null
  Step 'the bridge exits 0 when its input ends' {
    Assert ($code -eq 0) "engine-mcp exited $code"
  }

  # ---- a role that may not write ------------------------------------------------------------

  Step 'under qa: read-only tools offered and answering, writing tools withheld and refused' {
    if ($Connect) { Skip 'an attached bridge is governed by the host''s roles (--roles is refused with --connect); the spawned run covers them' }
    $doc = Need 'Doc'
    $qa = Start-Mcp 'qa' @('--roles', $Roles, '--role', 'qa')
    $ctx.QaServer = $qa
    $init = Send-Rpc $qa 'initialize' @{ protocolVersion = $ProtocolVersion; capabilities = @{}; clientInfo = @{ name = 'mcp-check'; version = '1' } } -Timeout 600
    Assert ($null -ne (Get-Prop $init.Answer 'result')) "initialize under qa: $($init.Text)"
    Send-Rpc $qa 'notifications/initialized' $null -Notification
    $list = Send-Rpc $qa 'tools/list' @{}
    $offered = @{}
    foreach ($t in @(Get-Prop $list.Answer 'result.tools')) { $offered[[string](Get-Prop $t 'name')] = $t }
    $ctx.QaListBytes = $list.Bytes
    $ctx.QaTools = $offered.Count
    $all = Need 'Tools'
    $withheld = @($all.Keys | Where-Object { -not $offered.ContainsKey($_) } | Sort-Object)
    Assert ($withheld.Count -gt 0) 'qa is offered every tool'
    foreach ($n in $withheld) {
      Assert ((Get-Prop $all[$n] 'annotations.readOnlyHint') -ne $true) "qa is not offered $n, which is read-only"
    }
    foreach ($n in @('create_object', 'set_property', 'apply', 'undo', 'delete_object')) {
      Assert (-not $offered.ContainsKey($n)) "qa is offered $n"
    }
    $refused = Invoke-Tool $qa 'create_object' @{ session = 'x'; type = 'engine.content.AssetProvenance'; attribution = (Rationale 'qa may not') }
    Assert (-not $refused.Ok) 'qa called create_object'
    $ctx.QaRefusal = Show $refused
    # The read-only loop: open the document the agent wrote, read it, check it.
    $o = Invoke-Tool $qa 'open_session' @{ path = $doc }
    Assert-Ok $o 'open_session under qa'
    $s = [string](Get-Prop $o.Data 'session')
    foreach ($read in @(@('objects', @{ session = $s }), @('journal', @{ session = $s }),
                        @('validate', @{ session = $s }), @('layers', @{ session = $s }),
                        @('describe', @{ type = 'engine.content.AssetProvenance' }))) {
      $r = Invoke-Tool $qa $read[0] $read[1]
      Assert-Ok $r "$($read[0]) under qa"
    }
    $objects = Invoke-Tool $qa 'objects' @{ session = $s }
    Assert ((Get-Prop $objects.Data 'total') -eq 1) 'qa does not see the object the agent made'
    "$($offered.Count) of $($all.Count) tools offered ($($list.Bytes) bytes); $($withheld.Count) withheld; a call to create_object: $($ctx.QaRefusal)"
  }
  if ($ctx.ContainsKey('QaServer')) { $null = Stop-Mcp $ctx.QaServer; $ctx.Remove('QaServer') }

  Step 'under designer: a direct edit is refused by the host (1008) with a hint to propose' {
    if ($Connect) { Skip 'an attached bridge is governed by the host''s roles; the spawned run covers them' }
    $doc = Need 'Doc'
    $id = Need 'Object'
    $designer = Start-Mcp 'designer' @('--roles', $Roles, '--role', 'designer')
    $ctx.DesignerServer = $designer
    $null = Send-Rpc $designer 'initialize' @{ protocolVersion = $ProtocolVersion; capabilities = @{}; clientInfo = @{ name = 'mcp-check'; version = '1' } } -Timeout 600
    Send-Rpc $designer 'notifications/initialized' $null -Notification
    $o = Invoke-Tool $designer 'open_session' @{ path = $doc }
    Assert-Ok $o 'open_session under designer'
    $s = [string](Get-Prop $o.Data 'session')
    $e = Invoke-Tool $designer 'set_property' @{ session = $s; id = $id; name = 'generator'; value = 'direct'; attribution = (Rationale 'a designer may not edit directly') }
    Assert (-not $e.Ok) 'a designer edited the base layer directly'
    Assert ((Get-Prop $e.Data 'error.code') -eq 1008) "expected 1008 Forbidden: $(Show $e)"
    Assert ($e.Text -match 'propose_layer') "the refusal does not point at propose_layer: $(Show $e)"
  }
  if ($ctx.ContainsKey('DesignerServer')) { $null = Stop-Mcp $ctx.DesignerServer; $ctx.Remove('DesignerServer') }
} finally {
  if ($null -ne $bridge) { $null = Stop-Mcp $bridge }
  foreach ($k in @('QaServer', 'DesignerServer')) { if ($ctx.ContainsKey($k)) { $null = Stop-Mcp $ctx[$k] } }
  if ($null -ne $watch) {
    try { $null = $watch.Ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'done', (New-WsToken)).GetAwaiter().GetResult() } catch { }
    $watch.Ws.Dispose()
  }
  if ($null -ne $hostProcess) {
    # Our own child: its input closed ends it, as it ends every engine-host.
    try { $hostProcess.StandardInput.Close() } catch { }
    if (-not $hostProcess.WaitForExit(60000)) { $hostProcess.Kill($true) }
    $log = if ($hostErr.Wait(5000)) { $hostErr.Result } else { '' }
    [IO.File]::WriteAllText((Join-Path $Scratch 'host.stderr.log'), $log, $utf8)
  }
}

# ---- the table --------------------------------------------------------------------------------

$rows = foreach ($g in ($calls | Group-Object Tool | Sort-Object Name)) {
  $ms = $g.Group | Measure-Object Ms -Maximum -Sum
  $bytes = $g.Group | Measure-Object Bytes -Maximum
  $schema = if ($ctx.ContainsKey('SchemaBytes') -and $ctx.SchemaBytes.ContainsKey($g.Name)) { $ctx.SchemaBytes[$g.Name] } else { $null }
  [pscustomobject]@{
    Tool = $g.Name; Calls = $g.Count
    'Max ms' = [math]::Round($ms.Maximum, 1); 'Mean ms' = [math]::Round($ms.Sum / $g.Count, 1)
    'Schema B' = $schema; 'Max result B' = [int64]$bytes.Maximum
  }
}
Write-Host ''
Write-Host ($rows | Format-Table -AutoSize | Out-String -Width 160).TrimEnd()
if ($ctx.ContainsKey('ListBytes')) {
  Write-Host ''
  Write-Host ("tools/list: {0} tools, {1} bytes (~{2} tokens at 4 bytes a token)" -f $ctx.Tools.Count, $ctx.ListBytes, [int]($ctx.ListBytes / 4))
  $largest = $ctx.SchemaBytes.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5
  Write-Host ("largest input schemas: " + (($largest | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '))
}
if ($ctx.ContainsKey('QaListBytes')) {
  Write-Host ("tools/list under qa: {0} tools, {1} bytes" -f $ctx.QaTools, $ctx.QaListBytes)
}
if ($ctx.ContainsKey('EventMs')) {
  Write-Host ("attached: an edit through the bridge reached a second client as event.journal {0:N1} ms after the tools/call; the outliner's doc.objects took {1:N1} ms more" -f $ctx.EventMs, $ctx.OutlinerMs)
}

$failed = @($steps | Where-Object Status -eq 'FAIL')
$skipped = @($steps | Where-Object Status -eq 'skip')
Write-Host ''
Write-Host ("mcp-check: {0} steps, {1} failed, {2} skipped" -f $steps.Count, $failed.Count, $skipped.Count)
if ($failed.Count -gt 0) { $exit = 1 }

if ($Report) {
  $out = [ordered]@{
    mcp = $Mcp; protocol_version = $ProtocolVersion
    steps = @($steps); calls = @($calls); tools = @($rows)
    tools_list_bytes = $(if ($ctx.ContainsKey('ListBytes')) { $ctx.ListBytes } else { $null })
    schema_bytes = $(if ($ctx.ContainsKey('SchemaBytes')) { $ctx.SchemaBytes } else { $null })
    qa_tools_list_bytes = $(if ($ctx.ContainsKey('QaListBytes')) { $ctx.QaListBytes } else { $null })
  }
  [IO.File]::WriteAllText($Report, ($out | ConvertTo-Json -Depth 8), $utf8)
}

if ($ownScratch -and -not $KeepScratch -and $exit -eq 0) {
  Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue
} elseif ($ownScratch) {
  Write-Host "mcp-check: kept $Scratch"
}
exit $exit
