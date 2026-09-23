#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Build and test the Linux presets in a container, from a Windows checkout.

.DESCRIPTION
  tools/linux-build.ps1 [-Preset <name>|all] [-Test] [-Filter <regex>] [-Jobs <n>]
                        [-Shell] [-Rebuild] [-Docs [-Base <rev>]] [-Prune] [-Sync:$false]
                        [-NoWait] [-Offline]

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
    -Base         the revision -Docs diffs against. Default `main`.
    -Prune        remove this checkout's two volumes and report what they held, then exit. The
                  shared dependency cache is left alone; see docs/ci/local-linux.md.
    -Sync:$false  skip the source sync and build what is already in the volume.
    -NoWait       if another build holds the lock, say who and exit 2 instead of waiting.
    -Offline      run the build containers with --network none: proof that everything this
                  build needs is already in the dependency cache, and a failure if it is not.

  Exit status is the build's, or the tests' when -Test is given. With `all`, every preset is
  attempted and the status is the first failure — nothing stops early, because the second
  compiler's opinion is the reason to run four of them.

.NOTES
  The Windows checkout is mounted **read-only** and rsync'd into a named volume; nothing in the
  container can write to it, and the Windows build trees are never touched. Building over a
  bind mount of an NTFS checkout is what this avoids: WSL2 reaches Windows files over a 9p
  share, and a build of this tree is hundreds of thousands of small-file operations.
#>
[CmdletBinding()]
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
  [switch]$Sync = $true,
  [switch]$NoWait,
  [switch]$Offline
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Dockerfile = Join-Path $PSScriptRoot 'ci/linux.Dockerfile'

# The four ci.yml runs, in ci.yml's order.
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

function Get-ShortHash([string]$text) {
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try { return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant().Substring(0, 12) }
  finally { $sha.Dispose() }
}

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
# build directory with another one. The path is the identity.
$VolumeId = Get-ShortHash $Root.ToLowerInvariant()
$SrcVolume = "engine-linux-src-$VolumeId"
$DepsVolume = "engine-linux-deps-$VolumeId"
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
    throw 'docker not found on PATH. Install Docker Desktop and make sure the Linux engine is running.'
  }
  & docker version --format '{{.Server.Os}}/{{.Server.Arch}}' 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'the Docker daemon is not reachable; start Docker Desktop.' }
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

function Ensure-Volumes {
  foreach ($v in @($SrcVolume, $DepsVolume, $FetchVolume)) {
    & docker volume inspect $v 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      Write-Step "creating volume $v"
      & docker volume create $v | Out-Null
    }
  }
}

function Invoke-Sync([string]$image) {
  Write-Step "syncing $Root into $SrcVolume"
  $excludeArgs = ($SyncExcludes | ForEach-Object { "--exclude=$_" }) -join ' '
  # --delete so a file removed on the host disappears in the volume; excluded paths are
  # protected from it by default, which is what keeps /src/build alive across syncs.
  $syncScript = "rsync -a --delete $excludeArgs /host/ /src/ && echo ""sync: `$(find /src -type f -not -path '/src/build/*' | wc -l) files"""
  $script:CurrentContainer = "engine-linux-$VolumeId-$PID-sync"
  $syncArgs = @('run', '--rm', '--name', $script:CurrentContainer, '--label', "$RoleLabel=sync",
                '-v', "${HostMount}:/host:ro", '-v', "${SrcVolume}:/src",
                $image, 'bash', '-lc', $syncScript)
  & docker @syncArgs
  $status = $LASTEXITCODE
  $script:CurrentContainer = $null
  if ($status -ne 0) { throw "source sync failed ($status)" }
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

function Invoke-Prune {
  $failed = 0
  foreach ($v in @($SrcVolume, $DepsVolume)) {
    $size = & docker volume inspect $v --format '{{.Mountpoint}}' 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Host "$v does not exist"; continue }
    # A volume a container still holds cannot be removed, and docker says so on stderr; let that
    # reach the caller rather than reporting a removal that did not happen.
    & docker volume rm $v | Out-Null
    if ($LASTEXITCODE -ne 0) {
      Write-Host "could not remove $v (a container may still be using it)" -ForegroundColor Red
      $failed = 1
    } else {
      Write-Host "removed $v (was at $size)"
    }
  }
  Write-Host 'The images are shared between checkouts and are left alone; remove them with'
  Write-Host "  docker image rm $(Get-ImageTag 'desktop') $(Get-ImageTag 'headless')"
  Write-Host "So is the dependency cache, $FetchVolume; every checkout's next configure refetches if it goes:"
  Write-Host "  docker volume rm $FetchVolume"
  exit $failed
}

# --- run ---------------------------------------------------------------------------------------

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
