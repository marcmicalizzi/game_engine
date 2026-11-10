# The rules of tools/ci/check-no-listeners.ps1, as functions over data so they can be tested with no
# real listener, no firewall prompt and no engine binary (tools/ci/check-no-listeners.Tests.ps1,
# CTest `tools.check_no_listeners`). The script does the I/O: it copies, starts, samples and ends;
# everything it decides is decided here. See docs/ci/self-hosted-runners.md,
# "tools/ci/check-no-listeners.ps1".

Set-StrictMode -Version Latest

# Every pending firewall prompt is a PickerHost.exe whose command line names the dialog server; the
# command line is what tells it apart from the other things Windows runs under the same image name.
# `-Processes` is for the tests: a list of objects with ProcessId and CommandLine.
function Get-FirewallPrompts {
  param([object[]]$Processes)
  if ($null -eq $Processes) {
    $Processes = @(Get-CimInstance Win32_Process -Filter "Name='PickerHost.exe'" -ErrorAction SilentlyContinue)
  }
  @($Processes | Where-Object { $_.CommandLine -and $_.CommandLine -match 'FirewallNotificationDialogServer' })
}

# While a process is alive, what is it listening on? It needs no prompt and no fresh path, so it
# still means something on a machine that has already decided about every path in the tree.
function Get-ProcessListeners([int]$ProcessId) {
  $found = @()
  foreach ($c in @(Get-NetTCPConnection -OwningProcess $ProcessId -State Listen -ErrorAction SilentlyContinue)) {
    $found += "TCP $($c.LocalAddress):$($c.LocalPort)"
  }
  foreach ($u in @(Get-NetUDPEndpoint -OwningProcess $ProcessId -ErrorAction SilentlyContinue)) {
    $found += "UDP $($u.LocalAddress):$($u.LocalPort)"
  }
  return $found
}

# An endpoint as Get-ProcessListeners writes it, "TCP 127.0.0.1:8086" or "UDP ::1:5353". The whole
# of 127.0.0.0/8 and ::1 are loopback: nothing off the machine reaches them and the firewall does
# not ask about them. 0.0.0.0 and :: are every interface, which is the case this check exists for.
function Test-LoopbackAddress([string]$Endpoint) {
  if ($Endpoint -notmatch '^(TCP|UDP)\s+(.+):(\d+)$') { return $false }
  $address = $Matches[2].Trim('[', ']')
  return ($address -match '^127\.\d{1,3}\.\d{1,3}\.\d{1,3}$') -or ($address -eq '::1')
}

# Harmless arguments: enough to get the process through static initialization and into main, which
# is where a listener would already have been opened, and not enough to do any work. A Tracy client
# opens its socket before main runs, so even a binary that rejects these arguments has answered the
# question by the time it prints its usage. Nothing here opens a window: the two apps that draw are
# run `--offscreen`, and a bench lists its cases rather than running them.
function Get-ProbeArgs([string]$Name, [string]$Scratch) {
  switch -Regex ($Name) {
    '_bench$'       { return @('--list') }
    '^engine-view$' { return @('--offscreen', '--frames', '1', '--width', '64', '--height', '64') }
    # engine-host is a binary that listens on purpose (--listen, docs/subsystems/net.md): it is run
    # with its listener open on a fresh path, answering one request and exiting, so the check sees
    # the socket it opens and the firewall sees a new path bind it. Loopback raises no prompt. The
    # backslashes are for Start-Process, which hands the command line on as written.
    '^engine-host$' { return @('--listen', '127.0.0.1:0', '--request', '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"engine.ping\"}') }
    # engine-editor runs the same listener in process for its web shell (docs/subsystems/apps.md,
    # "engine-editor"): one offscreen frame of the viewport with the listener open, so the check sees
    # the socket it opens from a fresh path, and no window and no WebView2 appear.
    '^engine-editor$' { return @('--doc', (Join-Path $Scratch 'editor-doc'), '--create', '--no-shell', '--offscreen', '--listen', '127.0.0.1:0', '--frames', '1', '--width', '64', '--height', '64') }
    # The GPU lock's test probe (foundation/gpu_lock/tests/probe_main.cpp) takes the machine-wide
    # lock for any command it does not know, `--help` included, and so waited out the timeout behind
    # whoever held it (the first run, 2026-11-09). `cycles 0` takes and releases nothing.
    '^engine_gpu_lock_probe$' { return @('cycles', '0') }
    default         { return @('--help') }
  }
}

# The processes this run started, found by where their image lives: every copy is under the run's
# own directory, whose name is a fresh GUID, so a process whose image is under it is one of ours
# whatever its pid — a pid Windows has reused for somebody else's process names another image.
# `-Processes` is for the tests: a list of objects with ProcessId and ExecutablePath.
function Get-OwnProcesses {
  param([Parameter(Mandatory)][string]$Under, [object[]]$Processes)
  if ($null -eq $Processes) {
    $Processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
  }
  $prefix = [IO.Path]::GetFullPath($Under).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  @($Processes | Where-Object {
      $_.ExecutablePath -and
      ([IO.Path]::GetFullPath($_.ExecutablePath)).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    })
}

# The verdict over what a run saw. `-Observed` is a list of @{ Name; Endpoint }, `-NewPrompts` the
# prompts that appeared during the run with the binary each was blamed on (@{ ProcessId; Blame }).
# Exit 0 only when no endpoint was off loopback and no prompt appeared.
function Resolve-ListenerVerdict {
  param([object[]]$Observed = @(), [object[]]$NewPrompts = @(), [int]$Executables = 0)
  $lines = New-Object System.Collections.Generic.List[string]
  $notes = New-Object System.Collections.Generic.List[string]
  $open = New-Object System.Collections.Generic.List[string]
  foreach ($o in @($Observed)) {
    if (Test-LoopbackAddress $o.Endpoint) {
      $notes.Add("$($o.Name) listens on loopback: $($o.Endpoint) (allowed; no prompt, no reachability)")
    } else {
      $open.Add("$($o.Name) $($o.Endpoint)")
    }
  }
  if ($open.Count -gt 0) {
    $lines.Add("check-no-listeners: $($open.Count) non-loopback endpoint(s) observed")
    foreach ($l in $open) { $lines.Add("  $l") }
  }
  $prompts = @($NewPrompts)
  if ($prompts.Count -gt 0) {
    $lines.Add("check-no-listeners: $($prompts.Count) new firewall prompt(s)")
    foreach ($p in $prompts) { $lines.Add("  raised while running: $($p.Blame) (PickerHost pid $($p.ProcessId))") }
  }
  if ($open.Count -eq 0 -and $prompts.Count -eq 0) {
    $lines.Add("check-no-listeners: OK - no new firewall prompt from $Executables new executable paths, and no non-loopback endpoint")
    return [pscustomobject]@{ Code = 0; Lines = $lines.ToArray(); Notes = $notes.ToArray() }
  }
  $lines.Add('  The fix belongs in the binary or its dependency: bind 127.0.0.1, or do not listen.')
  return [pscustomobject]@{ Code = 1; Lines = $lines.ToArray(); Notes = $notes.ToArray() }
}

# The error mode every process this run starts inherits (CreateProcess passes the parent's on unless
# told not to): no "a DLL was not found" box from the loader, no "has stopped working" box from
# Windows Error Reporting, no "insert a disk" box. A copied binary that cannot start then exits with
# its status (0xC0000135) instead of waiting for a click on a desktop nobody is watching. The engine's
# own `platform::quiet_error_dialogs()` does the same for itself, but only once main runs, and the
# loader's box comes before main. Returns the mode it replaced, for Restore-ErrorMode.
function Set-QuietErrorMode {
  if (-not ('EngineNoListeners.Native' -as [type])) {
    Add-Type -Namespace EngineNoListeners -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetErrorMode(uint uMode);
'@
  }
  # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
  return [EngineNoListeners.Native]::SetErrorMode([uint32]0x8003)
}

function Restore-ErrorMode([uint32]$Mode) {
  if ('EngineNoListeners.Native' -as [type]) { [void][EngineNoListeners.Native]::SetErrorMode($Mode) }
}

Export-ModuleMember -Function Get-FirewallPrompts, Get-ProcessListeners, Test-LoopbackAddress, Get-ProbeArgs,
  Get-OwnProcesses, Resolve-ListenerVerdict, Set-QuietErrorMode, Restore-ErrorMode
