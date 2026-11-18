# The ssh options tools/remote-build.ps1 puts on every connection, and the bounded call its reach
# step makes. A module of its own so tools/linux-build.Tests.ps1 can hold both to a server on this
# machine that never answers (docs/ci/remote-linux.md, "A server that answers and then says nothing").
#
# Why both, measured 2026-11-18 against a loopback listener with Windows' OpenSSH 9.5:
#
# - **`ConnectTimeout` covers less than its name says.** A listener that accepts and never sends a
#   byte is given up after ConnectTimeout ("Connection timed out during banner exchange"). One that
#   sends its banner and then says nothing — a machine half up, sshd answering while what is behind
#   it is not — held ssh for as long as anyone waited: still running at 30 s with ConnectTimeout=5,
#   and three rows' runs sat at "reaching titanxp" for hours that way.
# - **`ServerAliveInterval` covers the key exchange too**, not only a session: the same listener with
#   ServerAliveInterval=2 and ServerAliveCountMax=2 is given up after 4.4 s, exit 255. So the
#   keepalive bounds a silent server on every connection the script makes, the build's included,
#   at Interval x CountMax seconds.
# - **A server that answers its keepalives and never finishes the command** is bounded by nothing
#   ssh has. The reach step's command is `uname; nproc; uptime`, which takes no time on a machine
#   that is up, so the reach step is also bounded as a process: past its seconds it is stopped and
#   counted as an attempt that found nobody.

function Get-RemoteSshOptions {
  [CmdletBinding()]
  param(
    [int]$ConnectSeconds = 20,
    # 15 s x 4: a server silent for a minute is gone. Long enough for a loaded machine's sshd and a
    # LAN's hiccup; short against the hours a hang cost.
    [int]$AliveSeconds = 15,
    [int]$AliveCount = 4
  )
  return @('-o', 'BatchMode=yes', '-o', "ConnectTimeout=$ConnectSeconds",
           '-o', "ServerAliveInterval=$AliveSeconds", '-o', "ServerAliveCountMax=$AliveCount")
}

# Runs a program with its output captured and its wall clock bounded. Past -Seconds the process
# (and anything it started) is stopped and the result says TimedOut; otherwise it carries the exit
# code. Output and errors are kept apart: the reach step parses one and quotes the other.
function Invoke-BoundedProcess {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [string[]]$Arguments = @(),
    [Parameter(Mandatory = $true)][int]$Seconds
  )
  $psi = [System.Diagnostics.ProcessStartInfo]::new($FilePath)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  # Nothing to read: a prompt (BatchMode should prevent one) gets end of input, not a wait.
  $psi.RedirectStandardInput = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  try {
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    $timedOut = -not $p.WaitForExit([Math]::Max(1, $Seconds) * 1000)
    if ($timedOut) {
      try { $p.Kill($true) } catch { }
    }
    $p.WaitForExit()
    $split = { param($t) @(($t -split "`r?`n") | Where-Object { $_ -ne '' }) }
    return [pscustomobject]@{
      TimedOut = $timedOut
      ExitCode = $(if ($timedOut) { $null } else { $p.ExitCode })
      Output   = & $split $out.Result
      Errors   = & $split $err.Result
    }
  } finally { $p.Dispose() }
}

Export-ModuleMember -Function Get-RemoteSshOptions, Invoke-BoundedProcess
