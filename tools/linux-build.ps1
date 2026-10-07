#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Build and test the Linux presets in a container, from a Windows checkout.

.DESCRIPTION
  tools/linux-build.ps1 [-Preset <name>|all] [-Test] [-Filter <regex>] [-Jobs <n>]
                        [-Shell] [-Rebuild] [-Docs [-Base <rev>]] [-Sync:$false]
                        [-NoWait] [-Offline]
  tools/linux-build.ps1 -Prune [-Stale [-KeepUnknown] [-Base <rev>]] [-WhatIf]

  The same toolchain `.github/workflows/ci.yml`'s `linux` job runs on — Ubuntu 24.04, the
  identical apt list, CMake 3.28, Clang 18, GCC 13, PowerShell 7 — in a container built from
  tools/ci/linux.Dockerfile, so GCC and Clang verification does not depend on GitHub being
  willing to run a job. docs/ci/local-linux.md is the long form, with the measured times: a warm
  run is minutes; the **first run of a fresh checkout** compiles every third-party dependency and
  is not.

  **One build at a time on this machine, whoever started it.** A build takes the machine-wide
  build lock first ($env:ENGINE_LINUX_BUILD_LOCK, default D:\workspace\linux-build.lock; the
  GPU-LOCK.md protocol, implemented once in tools/lib/MachineLock.psm1) and holds it until its
  last container has exited. A second invocation waits and says whom it is waiting for; -NoWait
  makes it fail at once with exit 2 instead. Three cold builds at once took over two and a half
  hours on 2026-09-20 and ran the 16 GB WSL VM out of memory; one after another they are three
  times one. The checkout is synced when the lock is taken, not when the command started, so
  what gets built is the tree as it is when the wait ends. -Shell and -Docs take no lock.

  **Downloaded dependency sources are shared by every checkout** (the engine-linux-fetch-cache
  volume, keyed by the dependency pins through tools/ci/fetch-cache.cmake), so a fresh worktree's
  first configure downloads nothing another checkout has already fetched. Build trees stay per
  checkout.

  Presets: linux-clang-debug, linux-gcc-release, linux-clang-minimal, linux-clang-no-ecs, or
  `all` for the four in that order — the same four, in the same order, as ci.yml's matrix.
  Any other Linux preset in CMakePresets.json works too (linux-clang-asan, for instance).

  The two **headless** presets — linux-server and linux-server-debug, which set
  ENGINE_WINDOW_BACKENDS=none for the Sandy Bridge GPU server — are built in the Dockerfile's
  `headless` target instead, which has no X11, Wayland, Mesa or libudev development package at
  all. Nothing has to be asked for: the preset decides the image. See docs/ci/remote-linux.md.

  Commands:
    -Test         also run CTest for the preset after the build (build alone is the default).
    -Filter       a regex on test names, passed to `ctest -R`.
    -Jobs         parallel compile jobs; default 8. See "Why eight" in docs/ci/local-linux.md.
    -Rebuild      delete build/<preset> inside the volume first. Downloaded dependencies live in
                  a separate volume and survive, so this costs a compile, not a download.
    -Shell        drop into an interactive bash in the container with the volumes mounted.
    -Docs         run tools/docs-gate.sh and tools/docs-check.sh in the container and nothing
                  else. The container has no git history, so the host's git produces the changed
                  file list and the commit messages and they are mounted read-only at /gate —
                  the same --files-from/--messages-from shape ci.yml uses for a pull request.
    -Base         the revision -Docs diffs against, and the one -Prune -Stale calls a worktree
                  "merged" into. Default `main`.
    -Prune        remove this checkout's two volumes and report what they held, then exit. **Run
                  it from your worktree before you hand it back**, once you are done with
                  containers: nothing else removes a checkout's volumes, and on 2026-10-07 250 of
                  them held 568 GB. The shared dependency cache and the images are left alone.
    -Stale        with -Prune: sweep the machine instead of this checkout. Removes every
                  engine-linux-src-* and engine-linux-deps-* volume that is stale — its checkout is
                  gone from disk; or its checkout is a worktree (not the main checkout, not locked,
                  no uncommitted changes) whose HEAD -Base contains, or whose every own commit
                  -Base holds by patch (a rebase merge, `git cherry`); or nothing can name its
                  checkout (no label, and its id is no listed checkout's). A volume any container
                  holds is kept and the container named; nothing else on the daemon is touched.
                  One line per volume — name, size, checkout, why — and the total. Named for the
                  rule rather than the scope: -All would read as "every volume", which is exactly
                  what it must never mean. docs/ci/local-linux.md, "Volumes that outlive their
                  checkout", has the rule and why each part of it is there.
    -KeepUnknown  with -Stale: keep the volumes whose checkout nothing can name.
    -WhatIf       with -Prune: print the same lines and remove nothing. Run it first.
    -Sync:$false  skip the source sync and build what is already in the volume.
    -NoWait       if another build holds the lock, say who and exit 2 instead of waiting.
    -Offline      run the build containers with --network none: proof that everything this
                  build needs is already in the dependency cache, and a failure if it is not.

  Exit status is the build's, or the tests' when -Test is given. With `all`, every preset is
  attempted and the status is the first failure — nothing stops early, because the second
  compiler's opinion is the reason to run four of them. -Prune exits 1 if docker refused a removal.
  Docker or git unreachable, or a switch given without the one it belongs to: one line, exit 1.

.NOTES
  The Windows checkout is mounted **read-only** and rsync'd into a named volume; nothing in the
  container can write to it, and the Windows build trees are never touched. Building over a
  bind mount of an NTFS checkout is what this avoids: WSL2 reaches Windows files over a 9p
  share, and a build of this tree is hundreds of thousands of small-file operations.

  The sync copies a file into the build's tree only when its **bytes** changed, and gives it the
  time of the copy, so a file edited while a build was running is always recompiled by the next
  one; see Invoke-Sync and "The sync" in docs/ci/local-linux.md.
#>
# SupportsShouldProcess for -WhatIf, which only -Prune honours; a build refuses it.
[CmdletBinding(SupportsShouldProcess)]
param(
  [string]$Preset = 'all',
  [switch]$Test,
  [string]$Filter,
  [int]$Jobs = 8,
  [switch]$Shell,
  [switch]$Rebuild,
  [switch]$Docs,
  [string]$Base = 'main',
  [switch]$Prune,
  [switch]$Stale,
  [switch]$KeepUnknown,
  [switch]$Sync = $true,
  [switch]$NoWait,
  [switch]$Offline
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib/LinuxVolumes.psm1') -Force

# The failures a caller can do something about — no docker, no daemon, a switch without the one it
# belongs to — end in one line on stderr and exit 1, not a PowerShell error record: an agent reads
# the first line, and a stack trace makes it read the wrong one.
function Stop-WithLine([string]$text) {
  [Console]::Error.WriteLine("linux-build: $text")
  exit 1
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Dockerfile = Join-Path $PSScriptRoot 'ci/linux.Dockerfile'

# The four ci.yml compile-and-test runs, in ci.yml's order. ci.yml's fifth Linux job, the sanitizer
# preset linux-clang-asan, is not in `all`: it is a slower build of the same tree that the merge does
# not wait for, and `-Preset linux-clang-asan` runs it here (docs/ci/what-to-run.md, "Sanitizers").
$AllPresets = @('linux-clang-debug', 'linux-gcc-release', 'linux-clang-minimal', 'linux-clang-no-ecs')

# The presets that must be built where no display development package exists. They are named
# rather than derived from the preset file because reading ENGINE_WINDOW_BACKENDS out of
# CMakePresets.json would mean parsing preset inheritance here, and a two-entry list that a new
# headless preset has to be added to is the smaller thing to get wrong.
$HeadlessPresets = @('linux-server', 'linux-server-debug')

# Source paths that must never reach the container: the Windows build trees (which would be
# overwritten by a Linux configure and are the thing we are protecting), the derived-data cache
# and sample assets (large, rebuildable, and no test needs them), the agent worktree metadata,
# and .git — which in a worktree is a *file* pointing at the main repository's administrative
# directory and would arrive broken. Nothing in the build or the test suite reads git history;
# tools/docs-gate.test.sh builds its own throwaway repositories.
$SyncExcludes = @(
  '/build/', '/out/', '/ddc/', '/content/samples/', '/content/golden/local/',
  '/.claude/', '/.git', '/.vs/', '/.vscode/', '/.idea/', '/.cache/', 'compile_commands.json'
)

function Write-Step([string]$text) { Write-Host "== $text" -ForegroundColor Cyan }

# docker is always called as `& docker @array` with the status read from $LASTEXITCODE on the
# next line, never through a wrapper that returns it: a PowerShell function's return value is
# its whole output stream, so a wrapper would swallow the build log into the status. The array
# splat also keeps docker's own -v and -w from being bound as PowerShell parameters.
#
# Get-ShortHash, the volume names and labels, and the sweep's decision live in
# tools/lib/LinuxVolumes.psm1, so that tools/linux-build.Tests.ps1 tests them without a daemon.

# The image tag is the Dockerfile's own hash **and the target**, so an edit to the toolchain
# produces a different image rather than a stale one that happens to share a name, and the
# headless image can never be mistaken for the desktop one — which would defeat the point of
# having it. Two worktrees with the same Dockerfile share both images and build each once.
$DockerfileHash = Get-ShortHash ([System.IO.File]::ReadAllText($Dockerfile))
function Get-ImageTag([string]$target) { "engine-linux-ci-${target}:$DockerfileHash" }

function Get-TargetForPreset([string]$name) {
  if ($HeadlessPresets -contains $name) { return 'headless' }
  return 'desktop'
}

# Build volumes are per checkout: every agent worktree is a separate tree and must not share a
# build directory with another one. The path is the identity, and since 2026-10-07 each volume also
# carries it as a label (Ensure-Volumes), so a sweep can name the checkout without hashing.
$VolumeId = Get-CheckoutVolumeId $Root
$SrcVolume = Get-CheckoutVolumeName -Role src -Root $Root
$DepsVolume = Get-CheckoutVolumeName -Role deps -Root $Root
# The downloaded dependency *sources* are not: they are a function of the pins, and one volume on
# the machine holds them for every checkout and both image targets (tools/ci/fetch-cache.cmake).
$FetchVolume = 'engine-linux-fetch-cache'
$FetchCacheArgs = '-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=/src/tools/ci/fetch-cache.cmake -DENGINE_FETCH_CACHE=/fetch'

# Every container this script starts carries this label, with its role as the value, so a build
# can tell its own kind apart from anything else on the daemon — and tell a build it must wait for
# (another checkout's, or an orphan whose script was killed) from a shell or a docs check it need
# not. Build and sync containers are also named, per invocation, so that a build interrupted with
# Ctrl+C can remove the container it started instead of leaving it compiling after the lock that
# covered it has been released.
$RoleLabel = 'engine.linux-build'
$script:CurrentContainer = $null

# Docker on Windows wants forward slashes in a bind source.
$HostMount = $Root -replace '\\', '/'

function Ensure-Docker {
  if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Stop-WithLine 'docker is not on PATH; install Docker Desktop and start its Linux engine.'
  }
  & docker version --format '{{.Server.Os}}/{{.Server.Arch}}' 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Stop-WithLine 'the Docker daemon is not reachable; is Docker Desktop running?' }
}

# Builds the image for one target if it is not already there. It deliberately returns **nothing**:
# `docker build`'s log is this function's output stream, so a function that also returned the tag
# would return the whole build log with it. Callers ask `Get-ImageTag` for the name.
function Ensure-Image([string]$target) {
  $image = Get-ImageTag $target
  $existing = & docker image inspect $image --format '{{.Id}}' 2>$null
  if ($LASTEXITCODE -eq 0 -and $existing) { return }
  Write-Step "building $image from tools/ci/linux.Dockerfile (target $target)"
  # An empty build context: the Dockerfile copies nothing from the tree, and sending the
  # checkout as context would be pointless traffic over the 9p share.
  $ctx = Join-Path ([System.IO.Path]::GetTempPath()) "engine-linux-ctx-$DockerfileHash"
  New-Item -ItemType Directory -Force -Path $ctx | Out-Null
  try {
    & docker build -f $Dockerfile --target $target -t $image $ctx
    if ($LASTEXITCODE -ne 0) { throw "docker build failed ($LASTEXITCODE)" }
  } finally { Remove-Item -Recurse -Force $ctx -ErrorAction SilentlyContinue }
}

# Every volume is created with labels that say whose it is (Get-VolumeLabels): engine.root, the
# checkout as $Root spells it; engine.role, src, deps or fetch-cache; engine.scope, checkout or
# shared; engine.created, UTC. docker cannot label a volume that already exists, so one made before
# 2026-10-07 stays unlabelled until it is pruned and made again, and a sweep finds its checkout by
# hashing the repository's worktrees instead.
function Ensure-Volumes {
  $wanted = @(
    @{ Name = $SrcVolume; Role = 'src' },
    @{ Name = $DepsVolume; Role = 'deps' },
    @{ Name = $FetchVolume; Role = 'fetch-cache' }
  )
  foreach ($v in $wanted) {
    & docker volume inspect $v.Name 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { continue }
    Write-Step "creating volume $($v.Name)"
    $labels = Get-VolumeLabels -Role $v.Role -Root $Root
    $createArgs = @('volume', 'create')
    foreach ($k in $labels.Keys) { $createArgs += @('--label', "$k=$($labels[$k])") }
    & docker @createArgs $v.Name | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "docker volume create $($v.Name) failed ($LASTEXITCODE)" }
  }
}

# **A file reaches /src with a fresh mtime when, and only when, its bytes changed.** Ninja rebuilds
# an object when a source is newer than it, so the one thing the sync must never do is give /src a
# changed file with an *old* time — and `rsync -a`, which is what this was until 2026-09-24, did
# exactly that: it keeps the host's mtime, and a source edited on Windows while a container build
# was compiling its previous text is older than the object built from that text. The next run
# copied the new text across and compiled nothing (docs/ci/local-linux.md, "The sync"). The
# remote script had the same flaw and the same fix a day earlier (docs/ci/remote-linux.md).
#
# Two stages, chosen by measurement over `rsync --checksum` straight from the host, which is as
# correct but reads every byte over the 9p share on every run (+0.75 s today, and growing with the
# tree):
#   1. host -> /deps/.host-mirror with `rsync -a`: the host's mtimes kept, so rsync's quick check
#      (size and mtime) finds what changed from a stat per file, as before. `--modify-window=-1`
#      compares the nanoseconds too: by default rsync compares whole seconds, and a same-size edit
#      in the same second as the previous sync was never copied at all.
#   2. mirror -> /src with `--checksum` and without `-t`: a file is copied only if its bytes differ
#      from what the build compiled, and a copied file gets the time of the copy. Both sides are on
#      the VM's own disk, so reading them is tens of milliseconds.
# The mirror lives in this checkout's deps volume because nothing reads that volume but CMake's
# FETCHCONTENT_BASE_DIR, one directory per preset; -Prune removes it with the rest.
function Invoke-Sync([string]$image) {
  Write-Step "syncing $Root into $SrcVolume"
  $excludeArgs = ($SyncExcludes | ForEach-Object { "--exclude=$_" }) -join ' '
  # --delete so a file removed on the host disappears in the volume; excluded paths are
  # protected from it by default, which is what keeps /src/build alive across syncs. The second
  # stage takes the same excludes for that reason alone: the mirror never holds a build/.
  #
  # A single-quoted here-string with the excludes substituted afterwards, so that bash's `$` and
  # quotes reach bash as written.
  $syncScript = @'
set -eo pipefail
mkdir -p /deps/.host-mirror
rsync -a --delete --modify-window=-1 @EXCLUDES@ /host/ /deps/.host-mirror/
rsync -rlp --checksum --delete --out-format='%i %n' @EXCLUDES@ /deps/.host-mirror/ /src/ > /tmp/sync.log
copied=$(grep -c '^>f' /tmp/sync.log || true)
removed=$(grep -c '^\*deleting' /tmp/sync.log || true)
files=$(find /src -path /src/build -prune -o -type f -print | wc -l)
echo "sync: $files files, $copied copied, $removed removed"
if [ "$copied" -gt 0 ] && [ "$copied" -le 12 ]; then grep '^>f' /tmp/sync.log | cut -d' ' -f2- | sed 's/^/  copied: /'; fi
'@
  $syncScript = $syncScript.Replace('@EXCLUDES@', $excludeArgs)
  $script:CurrentContainer = "engine-linux-$VolumeId-$PID-sync"
  $syncArgs = @('run', '--rm', '--name', $script:CurrentContainer, '--label', "$RoleLabel=sync",
                '-v', "${HostMount}:/host:ro", '-v', "${SrcVolume}:/src", '-v', "${DepsVolume}:/deps",
                $image, 'bash', '-lc', $syncScript)
  $started = [DateTime]::UtcNow
  & docker @syncArgs
  $status = $LASTEXITCODE
  $script:CurrentContainer = $null
  if ($status -ne 0) { throw "source sync failed ($status)" }
  Write-Host ("   in {0:N1}s" -f ([DateTime]::UtcNow - $started).TotalSeconds)
}

function Get-RunArgs([string]$role) {
  $runArgs = @(
    '--rm',
    '--label', "$RoleLabel=$role",
    '-v', "${SrcVolume}:/src",
    '-v', "${DepsVolume}:/deps",
    '-v', "${FetchVolume}:/fetch",
    '-w', '/src',
    # Ninja and CTest both write plain text; without this the odd UTF-8 in the tree's comments
    # comes back mangled in the Windows console.
    '-e', 'LANG=C.UTF-8'
  )
  if ($Offline) { $runArgs += @('--network', 'none') }
  return $runArgs
}

# Containers from this toolchain's images that a build must not run beside: anything but a shell
# or a docs check, from any checkout, including one started by a copy of this script that predates
# the lock (it carries no label at all) and an orphan whose script was killed without its finally.
function Get-BlockingContainers {
  $format = '{{.ID}}|{{.Image}}|{{.Names}}|{{.Label "' + $RoleLabel + '"}}|{{.RunningFor}}'
  $lines = & docker ps --format $format 2>$null
  if ($LASTEXITCODE -ne 0) { return @() }
  $found = @()
  foreach ($line in @($lines)) {
    $f = "$line" -split '\|'
    if ($f.Count -lt 5 -or $f[1] -notlike 'engine-linux-ci*') { continue }
    if ($f[3] -eq 'shell' -or $f[3] -eq 'docs') { continue }
    # A build or sync container this script started is named engine-linux-<checkout>-<pid>-<what>,
    # and the pid is the script that owns it.
    $owner = if ($f[2] -match '^engine-linux-[0-9a-f]{12}-(\d+)-') { [int]$Matches[1] } else { 0 }
    $found += [pscustomobject]@{ Id = $f[0]; Image = $f[1]; Name = $f[2]; Role = $f[3]; For = $f[4]; OwnerPid = $owner }
  }
  return $found
}

# With the lock held, waits until no other build container is running. Returns false on -NoWait
# or on the timeout, with the containers named.
#
# A container whose owning script is no longer running is an orphan — its script was killed
# outright (a tool timeout kills a process tree; no finally runs), the daemon kept the container
# compiling, and nobody will ever read its output. Seen the first time a run here was stopped that
# way, on 2026-09-22: the lock would have expired ten minutes later as designed, and the orphan
# would have gone on holding eight cores through its configure, its build and its whole test
# suite. Such a container is removed rather than waited for. Anything else — an unlabelled
# container from an older copy of this script, or one whose owner is alive (or whose pid has been
# reused, which errs on the side of waiting) — is waited for.
function Wait-BlockingContainers([double]$timeoutMinutes) {
  $deadline = [DateTime]::UtcNow.AddMinutes($timeoutMinutes)
  $announced = ''
  while ($true) {
    $running = @(Get-BlockingContainers)
    foreach ($c in $running) {
      if ($c.OwnerPid -gt 0 -and -not (Get-Process -Id $c.OwnerPid -ErrorAction SilentlyContinue)) {
        Write-Host "linux-build: removing $($c.Name), an orphan whose linux-build.ps1 (pid $($c.OwnerPid)) is gone" -ForegroundColor Yellow
        & docker rm -f $c.Name 2>&1 | Out-Null
      }
    }
    $running = @(Get-BlockingContainers | Where-Object {
        $_.OwnerPid -eq 0 -or (Get-Process -Id $_.OwnerPid -ErrorAction SilentlyContinue) })
    if ($running.Count -eq 0) { return $true }
    $names = ($running | ForEach-Object { "$($_.Name) ($($_.Image), up $($_.For))" }) -join ', '
    $line = "linux-build: another build container is running outside the lock: $names. " +
            "If it is an orphan of a killed run, 'docker rm -f $($running[0].Name)' ends it."
    if ($NoWait -or [DateTime]::UtcNow -ge $deadline) { Write-Host $line -ForegroundColor Yellow; return $false }
    # Once per set of containers, not once per poll: the uptime in the line changes every time.
    $key = ($running | ForEach-Object { $_.Name }) -join ','
    if ($key -ne $announced) { Write-Host "$line Waiting." -ForegroundColor Yellow; $announced = $key }
    Start-Sleep -Seconds 20
  }
}

function Invoke-DocsChecks {
  # The two documentation checks, where CI runs them. The gate is a diff, and /src has no git
  # history by design, so the host's git answers the two questions it asks and the answers go in
  # read-only at /gate. `git -C` rather than a Push-Location: this script never changes the
  # caller's directory.
  $gate = Join-Path ([System.IO.Path]::GetTempPath()) "engine-docs-gate-$VolumeId"
  New-Item -ItemType Directory -Force -Path $gate | Out-Null
  try {
    # Three dots for the files: the diff a pull request shows, from the merge base, so commits
    # that landed on the base branch meanwhile are not reported as this branch's changes. Two
    # dots for the messages, which really are only this branch's commits.
    $files = & git -C $Root diff --name-only "$Base...HEAD"
    if ($LASTEXITCODE -ne 0) { throw "git diff $Base...HEAD failed; is -Base a revision this checkout has?" }
    $messages = & git -C $Root log --format=%B "$Base..HEAD"
    # **Written with LF.** `Set-Content` ends lines with CRLF on Windows, docs-gate.sh reads the
    # list with `read -r`, and the carriage return rides along into every path — so `*.md` stops
    # matching and the gate reports documentation owed for a change that carried it. It cost
    # twenty minutes here; it would cost the next person the same.
    [System.IO.File]::WriteAllText((Join-Path $gate 'files.txt'), (($files -join "`n") + "`n"))
    [System.IO.File]::WriteAllText((Join-Path $gate 'messages.txt'), (($messages -join "`n") + "`n"))
    Write-Step "documentation gate against $Base, and docs-check"
    $mount = $gate -replace '\\', '/'
    # The documentation checks read files; the display packages make no difference to them, so
    # they run in the desktop image whatever preset was asked for.
    $docsArgs = @('run', '--rm', '--label', "$RoleLabel=docs", '-v', "${SrcVolume}:/src", '-v', "${mount}:/gate:ro", '-w', '/src',
                  '-e', 'LANG=C.UTF-8', (Get-ImageTag 'desktop'), 'bash', '-lc',
                  'tools/docs-gate.sh --files-from /gate/files.txt --messages-from /gate/messages.txt && tools/docs-check.sh')
    & docker @docsArgs
    exit $LASTEXITCODE
  } finally { Remove-Item -Recurse -Force $gate -ErrorAction SilentlyContinue }
}

function Invoke-Preset([string]$name) {
  $lines = @()
  # -e: a configure or build that fails ends the run with its own status. Without it the script
  # went on to `ctest`, which ran the *previous* build's binaries, passed, and made the container's
  # exit status 0: a -Test run printed "ok" over a compile error (2026-09-23, docs/ci/local-linux.md).
  $lines += 'set -eo pipefail'
  if ($Rebuild) { $lines += "rm -rf /src/build/$name" }
  # The dependencies' *sources* come from the machine-wide cache at /fetch
  # (tools/ci/fetch-cache.cmake, keyed by the pins), so nothing already fetched by any checkout is
  # fetched again. Their *build* directories stay in this checkout's second volume, one directory
  # per preset, because a Debug tree cannot share one with a Release tree: -Rebuild then costs an
  # engine compile and not a third-party one.
  #
  # FETCHCONTENT_UPDATES_DISCONNECTED stops a git-based dependency re-running `git fetch` on a
  # reconfigure should one ever be populated the ordinary way (a configure without the cache).
  $lines += "cmake --preset $name -DFETCHCONTENT_BASE_DIR=/deps/$name -DFETCHCONTENT_UPDATES_DISCONNECTED=ON $FetchCacheArgs"
  # -k 0 is ci.yml's: keep going after a failure so one run reports every error in the tree
  # rather than the first file Ninja happened to reach.
  $lines += "cmake --build --preset $name -j $Jobs -- -k 0"
  if ($Test) {
    $testCmd = "ctest --preset $name"
    if ($Filter) { $testCmd += " -R '$Filter'" }
    $lines += $testCmd
  }
  $runScript = $lines -join "`n"

  # A headless preset is built where no display development package exists, so `none` is verified
  # rather than merely configured. Ensure-Image is idempotent and cheap once the image is there.
  $target = Get-TargetForPreset $name
  Ensure-Image $target
  $image = Get-ImageTag $target

  Write-Step "$name (jobs=$Jobs, container $target$(if ($Test) { ', with tests' })$(if ($Offline) { ', no network' }))"
  $started = [DateTime]::UtcNow
  $script:CurrentContainer = "engine-linux-$VolumeId-$PID-$name"
  $runArgs = @('run', '--name', $script:CurrentContainer) + (Get-RunArgs 'build') + @($image, 'bash', '-lc', $runScript)
  # The compiler's diagnostics are the product of this script, so they go to the caller's
  # streams as they arrive. The status therefore cannot be this function's return value — that
  # is its whole output stream — and is left in a script-scoped variable instead.
  & docker @runArgs
  $script:PresetStatus = $LASTEXITCODE
  $script:CurrentContainer = $null
  $elapsed = [DateTime]::UtcNow - $started
  $verdict = if ($script:PresetStatus -eq 0) { 'ok' } else { "FAILED ($script:PresetStatus)" }
  $colour = if ($script:PresetStatus -eq 0) { 'Green' } else { 'Red' }
  Write-Host ("-- {0}: {1} in {2:m\m\ ss\s}" -f $name, $verdict, $elapsed) -ForegroundColor $colour
}

function Invoke-Shell {
  # -Shell follows -Preset, so `-Preset linux-server -Shell` drops into the headless image and
  # `ls /usr/include/X11` answering "no such file" is the point.
  $target = Get-TargetForPreset $(if ($Preset -eq 'all') { $AllPresets[0] } else { $Preset })
  Ensure-Image $target
  $image = Get-ImageTag $target
  Write-Step "shell in $image ($SrcVolume at /src, $DepsVolume at /deps, $FetchVolume at /fetch)"
  # -it only when there is a terminal to attach: docker refuses `-t` with redirected input, and
  # "the input device is not a TTY" is a poor way to learn that this switch wants a console.
  $tty = if ([Console]::IsInputRedirected) { @() } else { @('-it') }
  $runArgs = @('run') + $tty + (Get-RunArgs 'shell') + @($image, 'bash')
  & docker @runArgs
  exit $LASTEXITCODE
}

# --- -Prune ------------------------------------------------------------------------------------
#
# The facts are gathered here and judged in tools/lib/LinuxVolumes.psm1 (Get-VolumeVerdict), which
# is a pure function so that tools/linux-build.Tests.ps1 can write any machine down and check the
# verdicts without a daemon. docs/ci/local-linux.md, "Volumes that outlive their checkout".

# The checkout volumes on the daemon, with their labels and sizes. `docker system df -v` is the only
# place docker reports a volume's size, and `docker volume inspect` the only place its labels come
# back as a map rather than a "k=v,k=v" string that a path with a comma in it would break.
function Get-VolumeFacts {
  $json = & docker system df -v --format '{{json .Volumes}}' 2>$null
  if ($LASTEXITCODE -ne 0) { Stop-WithLine "docker system df failed ($LASTEXITCODE)" }
  $sizes = @{}
  foreach ($v in @(("$json" | ConvertFrom-Json))) {
    if ($v -and "$($v.Name)" -match '^engine-linux-(src|deps)-') { $sizes["$($v.Name)"] = "$($v.Size)" }
  }
  $facts = @()
  if ($sizes.Count -eq 0) { return , $facts }
  $names = @($sizes.Keys | Sort-Object)
  # A volume removed between the two calls makes inspect fail for that name alone; what it did
  # return is still the answer for the rest.
  $lines = & docker volume inspect @names --format '{{json .}}' 2>$null
  foreach ($line in @($lines)) {
    if (-not "$line".TrimStart().StartsWith('{')) { continue }
    $o = "$line" | ConvertFrom-Json -AsHashtable
    $facts += New-VolumeFact -Name $o['Name'] -Labels $o['Labels'] -Size $sizes[$o['Name']]
  }
  return , $facts
}

# Which containers hold which volumes. docker refuses to remove a volume any container holds,
# running or stopped, so the sweep names the container rather than asking.
function Get-VolumeHolders {
  $lines = & docker ps -a --no-trunc --format '{{.Names}}|{{.State}}|{{.Mounts}}' 2>$null
  if ($LASTEXITCODE -ne 0) { Stop-WithLine "docker ps failed ($LASTEXITCODE)" }
  return ConvertFrom-ContainerMounts -Lines @($lines)
}

# The repository's checkouts, as the decision needs them. Merge state and uncommitted changes are
# asked of git only for a worktree that owns a volume and could be stale, so a machine with fifty
# worktrees and four volumes runs a handful of git commands, not a hundred. A root that only a label
# names (a worktree already removed, or another clone's checkout) is looked for on disk.
function Get-CheckoutFacts([object[]]$volumes) {
  $porcelain = & git -C $Root worktree list --porcelain 2>$null
  if ($LASTEXITCODE -ne 0) { Stop-WithLine "git worktree list failed in $Root; -Stale judges volumes by the repository's checkouts" }
  $ids = @{}
  $labelRoots = @()
  foreach ($v in @($volumes)) {
    if ("$($v.Name)" -match '-([0-9a-f]{12})$') { $ids[$Matches[1]] = $true }
    if ($v.Labels -and $v.Labels.Contains('engine.root') -and $v.Labels['engine.root']) { $labelRoots += "$($v.Labels['engine.root'])" }
  }
  $labelKeys = @{}
  foreach ($r in $labelRoots) { $labelKeys[(ConvertTo-RootKey $r)] = $true }

  $facts = @()
  $seen = @{}
  # Not wrapped in @(): the parser returns its array as one object, and @() would make it the
  # single element of another.
  $worktrees = ConvertFrom-WorktreePorcelain -Lines @($porcelain)
  foreach ($w in $worktrees) {
    if ($w.Bare) { continue }
    $croot = ConvertTo-CheckoutRoot $w.Path
    $key = ConvertTo-RootKey $croot
    $seen[$key] = $true
    $present = Test-Path -LiteralPath (Join-Path $croot '.git')
    $merged = $false
    $byPatch = $false
    $dirty = $false
    $owns = $ids.ContainsKey((Get-CheckoutVolumeId $croot)) -or $labelKeys.ContainsKey($key)
    if ($owns -and $present -and -not $w.IsMain -and -not $w.Locked -and $w.Head) {
      & git -C $Root merge-base --is-ancestor $w.Head $Base 2>$null
      $merged = ($LASTEXITCODE -eq 0)
      if (-not $merged) {
        # A merge that rebases the branch first lands its commits in the base as new commits, so
        # the worktree's own are never ancestors of it. `git cherry` compares them by patch: every
        # line '-' means every commit is there. A conflict resolved on the way changes the patch,
        # and the worktree is kept.
        $cherry = @(& git -C $Root cherry $Base $w.Head 2>$null)
        if ($LASTEXITCODE -eq 0 -and $cherry.Count -gt 0 -and @($cherry | Where-Object { "$_" -notmatch '^- ' }).Count -eq 0) {
          $merged = $true
          $byPatch = $true
        }
      }
      if ($merged) {
        # A status git cannot give counts as changes: the volume is kept rather than guessed about.
        $status = & git -C $croot status --porcelain 2>$null
        $dirty = ($LASTEXITCODE -ne 0) -or [bool]$status
      }
    }
    $facts += New-CheckoutFact -Root $croot -IsMain:$w.IsMain -Present $present -Branch $w.Branch -Head $w.Head `
                               -Merged $merged -MergedByPatch $byPatch -Locked $w.Locked -LockReason $w.LockReason -Dirty $dirty
  }
  foreach ($r in $labelRoots) {
    $key = ConvertTo-RootKey $r
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $facts += New-CheckoutFact -Root $r -Listed $false -Present (Test-Path -LiteralPath (Join-Path $r '.git'))
  }
  return , $facts
}

# One line per volume, stale ones first; removes the stale ones unless -WhatIf. A container that
# started after the decision was made still makes docker refuse, and the refusal is printed whole.
# Returns 1 if docker refused anything. Only Write-Host reaches the console: this function's output
# stream is its status.
function Invoke-VolumeRemoval([object[]]$verdicts) {
  $failed = 0
  $count = 0
  $bytes = 0.0
  $ordered = @($verdicts | Sort-Object @{ Expression = { -not $_.Stale } }, @{ Expression = { "$($_.Root)" } }, Name)
  foreach ($v in $ordered) {
    if (-not $v.Stale) { Write-Host (Format-VolumeVerdict $v); continue }
    if ($WhatIfPreference) {
      Write-Host (Format-VolumeVerdict $v -Action 'stale') -ForegroundColor Yellow
    } else {
      $out = & docker volume rm $v.Name 2>&1
      if ($LASTEXITCODE -ne 0) {
        Write-Host (Format-VolumeVerdict $v -Action 'FAILED') -ForegroundColor Red
        Write-Host "       docker: $(("$out" -replace '\s+', ' ').Trim())" -ForegroundColor Red
        $failed = 1
        continue
      }
      Write-Host (Format-VolumeVerdict $v -Action 'remove')
    }
    $count++
    if ($null -ne $v.Bytes) { $bytes += $v.Bytes }
  }
  if ($WhatIfPreference) {
    Write-Host ("-WhatIf: {0} volume(s), {1}, would be removed; nothing was." -f $count, (Format-DockerSize $bytes))
  } else {
    Write-Host ("reclaimed {0} from {1} volume(s)" -f (Format-DockerSize $bytes), $count)
  }
  return $failed
}

function Invoke-Prune {
  $volumes = Get-VolumeFacts
  $holders = Get-VolumeHolders
  if ($Stale) {
    & git -C $Root rev-parse --verify --quiet "$Base^{commit}" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Stop-WithLine "-Base '$Base' is not a revision of this repository" }
    $checkouts = Get-CheckoutFacts $volumes
    $verdicts = Get-VolumeVerdict -Volumes $volumes -Checkouts $checkouts -Holders $holders -CurrentRoot $Root `
                                  -Base $Base -KeepUnknown:$KeepUnknown
    Write-Step "engine-linux checkout volumes on this daemon$(if ($WhatIfPreference) { ' (-WhatIf: nothing is removed)' })"
    if ($verdicts.Count -eq 0) { Write-Host 'none'; exit 0 }
    exit (Invoke-VolumeRemoval $verdicts)
  }

  $verdicts = Get-OwnVolumeVerdict -Volumes $volumes -Root $Root -Holders $holders
  Write-Step "this checkout's volumes$(if ($WhatIfPreference) { ' (-WhatIf: nothing is removed)' })"
  foreach ($name in @($SrcVolume, $DepsVolume)) {
    if (-not ($verdicts | Where-Object { $_.Name -eq $name })) { Write-Host "$name does not exist" }
  }
  $failed = if ($verdicts.Count -gt 0) { Invoke-VolumeRemoval $verdicts } else { 0 }
  Write-Host 'The images are shared between checkouts and are left alone; remove them with'
  Write-Host "  docker image rm $(Get-ImageTag 'desktop') $(Get-ImageTag 'headless')"
  Write-Host "So is the dependency cache, $FetchVolume; every checkout's next configure refetches if it goes:"
  Write-Host "  docker volume rm $FetchVolume"
  Write-Host "Other checkouts' leftovers: tools/linux-build.ps1 -Prune -Stale -WhatIf"
  exit $failed
}

# --- run ---------------------------------------------------------------------------------------

if ($Stale -and -not $Prune) { Stop-WithLine '-Stale is a -Prune option: tools/linux-build.ps1 -Prune -Stale -WhatIf' }
if ($KeepUnknown -and -not $Stale) { Stop-WithLine '-KeepUnknown is a -Prune -Stale option' }
if ($WhatIfPreference -and -not $Prune) { Stop-WithLine '-WhatIf applies to -Prune only; a build has no dry run' }

Ensure-Docker
if ($Prune) { Invoke-Prune }
Ensure-Volumes

[string[]]$targets = if ($Preset -eq 'all') { $AllPresets } else { @($Preset) }

# The sync only needs rsync, which both targets have, so it runs in whichever image the first
# preset wants and a headless-only run never builds the desktop one.
$syncTarget = Get-TargetForPreset $targets[0]

# A shell and the documentation checks are not builds: they take no lock, and their containers
# are labelled so that a build does not wait for them either.
if ($Docs -or $Shell) {
  Ensure-Image $syncTarget
  if ($Sync) { Invoke-Sync (Get-ImageTag $syncTarget) }
  if ($Docs) { Ensure-Image 'desktop'; Invoke-DocsChecks }
  if ($Shell) { Invoke-Shell }
}

# --- the build lock --------------------------------------------------------------------------
#
# Taken before anything that costs the machine: the image build, the sync and every preset. The
# lease is short and refreshed from a background thread while the build runs, so a build whose
# script is killed holds the machine for ten minutes, not for as long as the build would have
# taken. On a machine without the lock's directory there is nobody to coordinate with; the build
# runs unlocked and says so.
$LockPath = Get-MachineLockPath -Kind linux-build
$LockTimeoutMinutes = 240
$purpose = "linux-build $($targets -join ',')$(if ($Test) { ' -Test' }) in $Root"
$lock = Enter-MachineLock -Path $LockPath -Purpose $purpose -LeaseMinutes 10 -Wait:(-not $NoWait) `
                          -TimeoutMinutes $LockTimeoutMinutes -Label 'linux-build'
switch ($lock.Outcome) {
  'NoDirectory' { Write-Warning "linux-build: $(Split-Path -Parent $LockPath) does not exist; building without the machine-wide lock" }
  'Busy' { Write-Host "linux-build: another build holds the lock: $(Format-MachineLockHolder $lock.Holder)" -ForegroundColor Yellow; exit 2 }
  'TimedOut' { Write-Host "linux-build: gave up after $LockTimeoutMinutes minutes; still held by $(Format-MachineLockHolder $lock.Holder)" -ForegroundColor Red; exit 3 }
}
$heartbeat = if ($lock.Handle) { Start-MachineLockHeartbeat -Handle $lock.Handle } else { $null }

$results = [ordered]@{}
$overall = 0
try {
  if (-not (Wait-BlockingContainers $LockTimeoutMinutes)) {
    $overall = 2
  } else {
    Ensure-Image $syncTarget
    if ($Sync) { Invoke-Sync (Get-ImageTag $syncTarget) }
    foreach ($p in $targets) {
      $script:PresetStatus = 0
      Invoke-Preset $p
      $results[$p] = $script:PresetStatus
      if ($script:PresetStatus -ne 0 -and $overall -eq 0) { $overall = $script:PresetStatus }
    }
  }
} finally {
  # Ctrl+C lands here with a container still compiling; the lock must not be released while it
  # is, or the next build starts beside it.
  if ($script:CurrentContainer) {
    Write-Host "linux-build: removing $($script:CurrentContainer)" -ForegroundColor Yellow
    & docker rm -f $script:CurrentContainer 2>&1 | Out-Null
  }
  Stop-MachineLockHeartbeat -Job $heartbeat
  Exit-MachineLock -Handle $lock.Handle
}

if ($targets.Count -gt 1 -and $results.Count -gt 0) {
  Write-Host ''
  Write-Step 'summary'
  foreach ($k in $results.Keys) {
    $v = $results[$k]
    Write-Host ("  {0,-24} {1}" -f $k, $(if ($v -eq 0) { 'ok' } else { "FAILED ($v)" })) `
      -ForegroundColor $(if ($v -eq 0) { 'Green' } else { 'Red' })
  }
}

exit $overall
