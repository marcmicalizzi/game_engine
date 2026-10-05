#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Build and test the engine on a remote Linux machine over SSH, from a Windows checkout.

.DESCRIPTION
  tools/remote-build.ps1 -Host <ssh alias> [-Preset <name>] [-Test] [-Filter <regex>]
                         [-Jobs <n>] [-Clean] [-Fetch <dir>] [-Sync:$false]

  The third way to build Linux here, beside the hosted runner and `tools/linux-build.ps1`'s
  container, and the only one that is a **real machine**: a named CPU, a real kernel, the
  distribution's own compilers, and — once its driver is back — a GPU. It exists because the
  container answers "does this compile under Clang 18 and GCC 13" and cannot answer "does this
  run on a Sandy Bridge Xeon built by GCC 14 and clang 22". docs/ci/remote-linux.md is the long
  form, including the rules that apply to the machine this is pointed at.

    -Host       an alias in this machine's SSH config, e.g. `titanxp`. Required. Key-only auth:
                this script never prompts and never handles a password.
    -Preset     a preset from CMakePresets.json. Default `linux-server` — GCC, RelWithDebInfo,
                x86-64-v2, ENGINE_WINDOW_BACKENDS=none, which is what a headless v2 server is.
    -Test       run CTest after the build.
    -Filter     a regex on test names, passed to `ctest -R`.
    -Jobs       parallel compile jobs. Default: the remote machine's own `nproc`.
    -Clean      delete the remote build directory for this preset first. The downloaded
                dependencies live outside it and survive, so this costs a compile, not a download.
    -Fetch      a local directory to copy the run's artefacts into: the CTest log (which carries
                every test binary's own output), the failed-test list, and the `gpu.adapters`
                report taken on the remote machine.
    -Sync:$false  build what is already on the remote machine.

  Exit status is the build's, or the tests' when -Test is given.

.NOTES
  **The sync, and why it is tar and not rsync.** Git for Windows ships `ssh`, `scp` and GNU `tar`;
  it does not ship `rsync`, and rsync needs a binary at *both* ends. So the working tree — tracked
  files plus untracked ones git is not ignoring, which is what makes uncommitted work testable —
  is listed by `git ls-files`, packed by `tar`, copied by `scp` and unpacked by the remote `tar`.
  That sends everything every time. At this tree's size (629 files, 8.4 MB) a full resend measures
  a second or two over the LAN, against the fraction of one an incremental rsync would take — a
  second, set against a dependency this machine would otherwise have to grow. Revisit it if the
  tree gains large binaries; `content/samples/` and `ddc/` are already git-ignored and never
  travel. A tarball rather than a pipe because **PowerShell has no input redirection**: there is
  no `cmd < file` in PowerShell 7, and routing one through `cmd.exe` to save a temporary file
  would put three quoting dialects in one line.

  **Deleting what vanished** is the half tar cannot do. The same file list goes across as a
  manifest and the remote removes every file under the tree that is not in it — `build/` excepted,
  since that is where its own output lives. Without it a file renamed on Windows would leave its
  old copy behind, still compiling, for as long as the remote directory lives.

  **Everything the remote runs is POSIX sh.** That machine has no PowerShell and is not going to
  get one, which is also why five CTest tests do not exist there; docs/ci/remote-linux.md lists
  them.
#>
[CmdletBinding()]
param(
  # `Host` cannot be a PowerShell variable name — $Host is an automatic, read-only, AllScope
  # variable and a parameter of that name fails at bind time — so the switch the caller types is
  # an alias onto a variable that is allowed to exist.
  [Parameter(Mandatory = $true)]
  [Alias('Host')]
  [string]$Server,
  [string]$Preset = 'linux-server',
  [switch]$Test,
  [string]$Filter,
  [int]$Jobs = 0,
  [switch]$Clean,
  [string]$Fetch,
  [switch]$Sync = $true
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SshOpts = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20')
$script:RemoteCpus = 0

function Write-Step([string]$text) { Write-Host "== $text" -ForegroundColor Cyan }

function Get-ShortHash([string]$text) {
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try { return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant().Substring(0, 12) }
  finally { $sha.Dispose() }
}

# One remote directory per local checkout, keyed by its path, for the reason `linux-build.ps1`
# gives its volumes one per checkout: every agent worktree is a different tree, and two of them
# sharing a build directory would each be rebuilding the other's source. The paths are written
# **unquoted or in double quotes** on the remote side so its shell expands $HOME; single quotes
# would send the four characters `$HOME`.
#
# There are two spellings of the same directory and both are needed. `$RemoteRoot` carries
# `$HOME` and is only ever used inside an ssh command, where a shell expands it. **scp does not
# run a shell**: since OpenSSH moved scp onto the SFTP protocol, `host:$HOME/x` is a literal
# path with a dollar sign in it and the copy fails with "No such file or directory". SFTP starts
# in the home directory, so scp gets the relative spelling instead.
$CheckoutId = Get-ShortHash $Root.ToLowerInvariant()
$RemoteRel = "game_engine-remote/$CheckoutId"
$RemoteRoot = '$HOME/' + $RemoteRel
$RemoteSrc = "$RemoteRoot/src"
$RemoteDeps = "$RemoteRoot/deps"

# ssh, with the output left on the caller's streams and the status in $LASTEXITCODE. It is never
# wrapped in a function that returns the status, because a PowerShell function returns its whole
# output stream and the build log would go with it.
function Invoke-Remote([string]$script) {
  $sshArgs = $SshOpts + @($Server, $script)
  & ssh @sshArgs
}

function Test-Reachable {
  Write-Step "reaching $Server"
  # That machine reboots without warning while its GPU driver is being rebuilt, so this is a short
  # retry rather than one attempt, and its failure message says so — an unreachable host is a fact
  # about the machine, never a verdict on the tree.
  #
  # It also reads `nproc` here rather than in a connection of its own, because **a connection to
  # this host costs ten and a half seconds before a byte of work happens**. See Get-RemoteJobs.
  for ($attempt = 1; $attempt -le 3; ++$attempt) {
    $sshArgs = $SshOpts + @($Server, 'uname -sr; nproc; uptime')
    $info = & ssh @sshArgs 2>&1
    if ($LASTEXITCODE -eq 0) {
      foreach ($line in $info) { Write-Host "   $line" }
      if ($info.Count -ge 2 -and $info[1] -match '^\s*(\d+)\s*$') {
        $script:RemoteCpus = [int]$Matches[1]
      }
      return
    }
    if ($attempt -lt 3) {
      $wait = 10 * $attempt
      Write-Host "   not reachable (attempt $attempt of 3); retrying in ${wait}s" -ForegroundColor Yellow
      Start-Sleep -Seconds $wait
    }
  }
  throw "$Server is not reachable over SSH after three attempts. It reboots on its own while its GPU driver is being rebuilt, so try again before treating this as a build failure; `ssh $Server` by hand says which it is."
}

function Invoke-SyncTree {
  Write-Step "syncing the working tree to ${Server}:$RemoteSrc"
  $started = [DateTime]::UtcNow
  $listPath = [System.IO.Path]::GetTempFileName()
  $manifestPath = [System.IO.Path]::GetTempFileName()
  $execPath = [System.IO.Path]::GetTempFileName()
  $tarPath = [System.IO.Path]::GetTempFileName()
  $megabytes = 0.0
  try {
    # Tracked files plus untracked-and-not-ignored ones: uncommitted work is the whole reason this
    # script exists rather than "push a branch and build it there".
    & git -C $Root ls-files -z --cached --others --exclude-standard > $listPath
    if ($LASTEXITCODE -ne 0) { throw "git ls-files failed ($LASTEXITCODE)" }

    $listBytes = [System.IO.File]::ReadAllBytes($listPath)
    $names = [System.Text.Encoding]::UTF8.GetString($listBytes).TrimEnd("`0") -split "`0"
    # The harness keeps per-session settings and every agent's git worktree under .claude/; they are
    # untracked, not ignored, and not part of the tree to build. Worse, a live agent worktree changes
    # while tar reads it ("file changed as we read it", exit 1), which is how a gate on 2026-09-23
    # never built anything. So .claude/ never goes across, and the list tar reads is rewritten.
    $names = @($names | Where-Object { $_ -ne '' -and $_ -notmatch '^\.claude(/|$)' })
    [System.IO.File]::WriteAllBytes($listPath, [System.Text.Encoding]::UTF8.GetBytes(($names -join "`0") + "`0"))
    if ($names.Count -eq 0 -or ($names.Count -eq 1 -and $names[0] -eq '')) {
      throw "git listed no files under $Root; is this a checkout?"
    }
    # The manifest is newline-separated so the remote's `sort` and `comm` can read it. A path
    # containing a newline would corrupt that silently and the wrong file would be deleted, so it
    # is refused instead. Git has never produced one here; a deletion is not the way to find out
    # that it can.
    foreach ($n in $names) {
      if ($n -match "[`r`n]") { throw "the path '$n' contains a newline; the manifest cannot represent it" }
    }
    [System.IO.File]::WriteAllText($manifestPath, (($names -join "`n") + "`n"))

    # **The executable bit does not survive the trip, and the CTest suite notices.** NTFS has no
    # mode bits, so git keeps them in its index and a `tar` built from the Windows working tree
    # records 0644 for everything. `tools/docs-gate.sh` is then extracted non-executable, and
    # `tools.docs_gate` — the one bash test that a machine with no pwsh still registers — fails
    # eighteen times with `Permission denied` and exit 126. The container never showed it because
    # Docker's bind mount of a Windows path reports everything as 0777.
    #
    # So the modes come from the one place that has them: `git ls-files -s`, whose first column is
    # 100755 for an executable blob. Five files in this tree. An *untracked* file has no index
    # entry and therefore arrives 0644 — acceptable, because a new script is committed before
    # anybody depends on it running here, and the alternative is guessing from a shebang.
    $staged = & git -C $Root ls-files -s
    if ($LASTEXITCODE -ne 0) { throw "git ls-files -s failed ($LASTEXITCODE)" }
    $executables = @()
    foreach ($line in $staged) {
      if ($line -match '^100755 [0-9a-f]+ \d+\t(.+)$') { $executables += $Matches[1] }
    }
    [System.IO.File]::WriteAllText($execPath, (($executables -join "`n") + "`n"))

    # **There are two tars on a Windows box and they disagree about drive letters.** Windows 11
    # ships bsdtar as C:\Windows\System32\tar.exe and it comes first on PATH in PowerShell; Git
    # for Windows ships GNU tar, which is what Git Bash finds. GNU tar reads `C:\Users\...` as the
    # host `C` and a path after the colon — its remote-archive heuristic — and needs
    # `--force-local` to be told otherwise, which bsdtar does not accept at all. So the flavour is
    # asked for rather than assumed. Everything else (`-c -f -C -T --null`) both understand.
    # `-C` precedes `-T` because tar applies options in the order it meets them.
    $tarFlavour = (& tar --version 2>&1 | Select-Object -First 1)
    $tarArgs = @()
    if ($tarFlavour -match 'GNU tar') { $tarArgs += '--force-local' }
    $tarArgs += @('-C', $Root, '--null', '-T', $listPath, '-c', '-f', $tarPath)
    & tar @tarArgs
    if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE); tar is '$tarFlavour'" }
    $megabytes = (Get-Item $tarPath).Length / 1MB

    # Into the home directory, not into $RemoteRel: the home directory always exists, so this
    # costs no `mkdir` connection of its own, and the unpack moves the three files into place.
    $scpArgs = $SshOpts + @('-q', $tarPath, $manifestPath, $execPath, "${Server}:")
    & scp @scpArgs
    if ($LASTEXITCODE -ne 0) { throw "scp of the tree failed ($LASTEXITCODE)" }

    $tarName = Split-Path -Leaf $tarPath
    $manifestName = Split-Path -Leaf $manifestPath
    $execName = Split-Path -Leaf $execPath
    # POSIX sh, because that is all the far side has. `comm -13` is "lines only in the second
    # file": the files that are there and should not be.
    #
    # A **single-quoted** here-string, with the three values substituted afterwards. A
    # double-quoted one would have PowerShell expand `$f` and `$(wc -l < ...)` before sh ever saw
    # them — and `<` is a reserved operator in PowerShell, so the script would not even parse.
    # Backslash is not PowerShell's escape character; this avoids needing one.
    $unpack = @'
set -e
mkdir -p "@ROOT@/src" "@ROOT@/deps"
cd "@ROOT@"
mv -f "$HOME/@MANIFEST@" src/.remote-manifest
mv -f "$HOME/@EXEC@" src/.remote-exec
# **Unpack beside the tree, then copy over only what changed.** `tar -x` restores each file's
# Windows mtime, and a source edited locally *while a remote build was compiling its previous
# version* is then older than the object built from the old text, so ninja never rebuilds it:
# on 2026-09-23 a warm-up build overlapped an edit and the next run linked a test against a
# stale library. A file whose bytes changed is copied with a fresh mtime, which is what make and
# ninja key on; an unchanged one keeps its old mtime, so nothing rebuilds for nothing. It is
# rsync's checksum rule, done with cmp because the far side has no rsync.
rm -rf .remote-incoming
mkdir .remote-incoming
tar -xf "$HOME/@TAR@" -C .remote-incoming
rm -f "$HOME/@TAR@"
(cd .remote-incoming && find . -type f -print) | while IFS= read -r f; do
  if [ -f "src/$f" ] && cmp -s ".remote-incoming/$f" "src/$f"; then continue; fi
  mkdir -p "src/$(dirname "$f")"
  cp -- ".remote-incoming/$f" "src/$f"
done
rm -rf .remote-incoming
cd src
while IFS= read -r f; do
  if [ -n "$f" ] && [ -f "$f" ]; then chmod +x -- "$f"; fi
done < .remote-exec
rm -f .remote-exec
LC_ALL=C sort -o .remote-manifest .remote-manifest
# **-prune, not -not -path.** `-not -path './build/*'` filters build/'s contents out of the
# *output* but find still walks all of it, and build/<preset> is a gigabyte of object files: the
# first version of this took 33 s a sync, almost all of it statting things it then discarded.
# -prune stops the descent. It costs a second.
find . -path ./build -prune -o -type f ! -name '.remote-*' -print | sed 's|^\./||' | LC_ALL=C sort > .remote-have
LC_ALL=C comm -13 .remote-manifest .remote-have > .remote-stale
while IFS= read -r f; do
  if [ -n "$f" ]; then rm -f -- "$f"; fi
done < .remote-stale
stale=$(wc -l < .remote-stale | tr -d " ")
kept=$(wc -l < .remote-manifest | tr -d " ")
find . -path ./build -prune -o -mindepth 1 -type d -empty -print | while IFS= read -r d; do
  rmdir -- "$d" 2>/dev/null || true
done
rm -f .remote-have .remote-stale
echo "   $kept files on the remote, $stale stale removed"
'@
    $unpack = $unpack.Replace('@ROOT@', $RemoteRoot).Replace('@MANIFEST@', $manifestName).Replace('@EXEC@', $execName).Replace('@TAR@', $tarName)
    Invoke-Remote $unpack
    if ($LASTEXITCODE -ne 0) { throw "the remote unpack failed ($LASTEXITCODE)" }
  } finally {
    Remove-Item -Force $listPath, $manifestPath, $execPath, $tarPath -ErrorAction SilentlyContinue
  }
  $elapsed = [DateTime]::UtcNow - $started
  Write-Host ("   {0:N1} MB in {1:N1}s" -f $megabytes, $elapsed.TotalSeconds)
}

# **Connections are the expensive part of this script, not bytes.** Measured against `titanxp`:
# `ssh host true` takes **10.6 s**, every time, from both the Windows OpenSSH client and Git for
# Windows' msys one — a ten-second timeout somewhere on the far side, and not this tree's business
# to fix on a machine we may not touch. Transfers are trivial beside it: 7.4 MB of tarball is a
# second. `ControlMaster` is the usual answer and **is not available here**: Windows OpenSSH does
# not implement multiplexing at all, and Git for Windows' msys ssh answers a `ControlPath` with
# `mux_client_request_session: read from master failed`.
#
# So the script spends connections like money. Reachability reads `nproc` on the way past; the
# three uploads go in one `scp`; unpacking, configuring, building, testing and the adapter report
# are one `ssh`; and `-Fetch` brings back a single tarball the remote built rather than four files.
# Four connections a run, five with -Fetch, against the eight a naive version used — a minute.
function Get-RemoteJobs {
  if ($Jobs -gt 0) { return $Jobs }
  if ($script:RemoteCpus -gt 0) { return $script:RemoteCpus }
  return 8
}

# --- run ---------------------------------------------------------------------------------------

foreach ($tool in @('ssh', 'scp', 'tar', 'git')) {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    throw "$tool not found on PATH. Git for Windows ships all four."
  }
}

Test-Reachable
if ($Sync) { Invoke-SyncTree }

$jobCount = Get-RemoteJobs
Write-Step "$Preset on $Server (jobs=$jobCount$(if ($Test) { ', with tests' }))"

# FETCHCONTENT_BASE_DIR outside the build tree, so -Clean costs a compile and not a 300 MB
# download, and FETCHCONTENT_UPDATES_DISCONNECTED so a reconfigure never re-fetches a pinned tag.
$lines = @('set -e', "cd `"$RemoteSrc`"")
# The compiler's and the tests' temporary files go beside the tree, on the volume the build is on,
# and not in the system's /tmp: on 2026-10-05 the server's root filesystem was full, a compile died
# with "error writing to /tmp/cc….s: No space left on device", and the build had done nothing wrong.
$lines += "mkdir -p `"$RemoteRoot/tmp`" && export TMPDIR=`"$RemoteRoot/tmp`""
if ($Clean) { $lines += "rm -rf `"build/$Preset`"" }
$lines += "cmake --preset $Preset -DFETCHCONTENT_BASE_DIR=`"$RemoteDeps/$Preset`" -DFETCHCONTENT_UPDATES_DISCONNECTED=ON"
$lines += "cmake --build --preset $Preset -j $jobCount -- -k 0"
if ($Test) {
  # `set -e` off around the tests: a failing suite is a result to collect artefacts for, not a
  # reason to stop before collecting them. Its status is kept and re-raised at the end.
  $lines += 'set +e'
  $testCmd = "ctest --preset $Preset"
  if ($Filter) { $testCmd += " -R '$Filter'" }
  $lines += $testCmd
  $lines += 'status=$?'
} else {
  $lines += 'status=0'
}
# The adapter report, always. It is the one artefact that says what this machine's GPU stack can
# do, it costs a process, and on a machine whose driver is being rebuilt the *failure* is the
# reading worth having. `|| true` because "no Vulkan loader" is today's expected answer and is not
# a build result.
$lines += "rm -rf `"$RemoteRoot/artifacts`" && mkdir -p `"$RemoteRoot/artifacts`""
$lines += "`"$RemoteSrc/build/$Preset/bin/engine-cli`" gpu.adapters --report `"$RemoteRoot/artifacts/adapters.json`" > `"$RemoteRoot/artifacts/adapters.txt`" 2>&1 || true"
if ($Fetch) {
  # The artefacts are collected and packed **here**, inside the run's own connection, so that
  # fetching them costs one `scp` rather than one per file. `cp` each with `|| true`: a build that
  # failed before CTest ran has no log, and that is a thing to report rather than to fail on.
  $lines += "for f in `"$RemoteSrc/build/$Preset/Testing/Temporary/LastTest.log`" `"$RemoteSrc/build/$Preset/Testing/Temporary/LastTestsFailed.log`"; do cp -f `"`$f`" `"$RemoteRoot/artifacts/`" 2>/dev/null || true; done"
  $lines += "tar -cf `"$RemoteRoot/artifacts.tar`" -C `"$RemoteRoot`" artifacts"
}
$lines += 'exit $status'

Invoke-Remote ($lines -join "`n")
$status = $LASTEXITCODE

if ($Fetch) {
  Write-Step "fetching artefacts into $Fetch"
  New-Item -ItemType Directory -Force -Path $Fetch | Out-Null
  # One tarball, packed by the run itself, rather than one `scp` per file — four connections at
  # ten and a half seconds each (see Get-RemoteJobs). `LastTest.log` inside it is every test
  # binary's own stdout and stderr, which is what makes a remote failure diagnosable without a
  # second round trip. A relative path: scp is SFTP and does not expand $HOME.
  $bundle = [System.IO.Path]::GetTempFileName()
  try {
    $scpArgs = $SshOpts + @('-q', "${Server}:$RemoteRel/artifacts.tar", $bundle)
    & scp @scpArgs
    if ($LASTEXITCODE -ne 0) {
      Write-Host '   nothing to fetch: the run did not get far enough to pack any' -ForegroundColor Yellow
    } else {
      $tarArgs = @()
      if ((& tar --version 2>&1 | Select-Object -First 1) -match 'GNU tar') { $tarArgs += '--force-local' }
      $tarArgs += @('-x', '-f', $bundle, '-C', $Fetch, '--strip-components', '1')
      & tar @tarArgs
      if ($LASTEXITCODE -ne 0) { throw "could not unpack the artefacts ($LASTEXITCODE)" }
      Get-ChildItem $Fetch | ForEach-Object { Write-Host "   $($_.Name)" }
    }
  } finally { Remove-Item -Force $bundle -ErrorAction SilentlyContinue }
}

Write-Host ("-- {0} on {1}: {2}" -f $Preset, $Server, $(if ($status -eq 0) { 'ok' } else { "FAILED ($status)" })) `
  -ForegroundColor $(if ($status -eq 0) { 'Green' } else { 'Red' })
exit $status
