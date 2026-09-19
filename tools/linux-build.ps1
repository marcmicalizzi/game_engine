#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Build and test the Linux presets in a container, from a Windows checkout.

.DESCRIPTION
  tools/linux-build.ps1 [-Preset <name>|all] [-Test] [-Filter <regex>] [-Jobs <n>]
                        [-Shell] [-Rebuild] [-Docs [-Base <rev>]] [-Prune] [-Sync:$false]

  The same toolchain `.github/workflows/ci.yml`'s `linux` job runs on — Ubuntu 24.04, the
  identical apt list, CMake 3.28, Clang 18, GCC 13, PowerShell 7 — in a container built from
  tools/ci/linux.Dockerfile, so GCC and Clang verification takes minutes and does not depend on
  GitHub being willing to run a job. docs/ci/local-linux.md is the long form.

  Presets: linux-clang-debug, linux-gcc-release, linux-clang-minimal, linux-clang-no-ecs, or
  `all` for the four in that order — the same four, in the same order, as ci.yml's matrix.
  Any other Linux preset in CMakePresets.json works too (linux-clang-asan, for instance).

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
    -Prune        remove this checkout's two volumes and report what they held, then exit.
    -Sync:$false  skip the source sync and build what is already in the volume.

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
  [switch]$Sync = $true
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Dockerfile = Join-Path $PSScriptRoot 'ci/linux.Dockerfile'

# The four ci.yml runs, in ci.yml's order.
$AllPresets = @('linux-clang-debug', 'linux-gcc-release', 'linux-clang-minimal', 'linux-clang-no-ecs')

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

# The image tag is the Dockerfile's own hash, so an edit to the toolchain produces a different
# image rather than a stale one that happens to share a name. Two worktrees with the same
# Dockerfile share the image and build it once.
$DockerfileHash = Get-ShortHash ([System.IO.File]::ReadAllText($Dockerfile))
$Image = "engine-linux-ci:$DockerfileHash"

# Volumes are per checkout: every agent worktree is a separate tree and must not share a build
# directory with another one. The path is the identity.
$VolumeId = Get-ShortHash $Root.ToLowerInvariant()
$SrcVolume = "engine-linux-src-$VolumeId"
$DepsVolume = "engine-linux-deps-$VolumeId"

# Docker on Windows wants forward slashes in a bind source.
$HostMount = $Root -replace '\\', '/'

function Ensure-Docker {
  if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'docker not found on PATH. Install Docker Desktop and make sure the Linux engine is running.'
  }
  & docker version --format '{{.Server.Os}}/{{.Server.Arch}}' 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'the Docker daemon is not reachable; start Docker Desktop.' }
}

function Ensure-Image {
  $existing = & docker image inspect $Image --format '{{.Id}}' 2>$null
  if ($LASTEXITCODE -eq 0 -and $existing) { return }
  Write-Step "building $Image from tools/ci/linux.Dockerfile"
  # An empty build context: the Dockerfile copies nothing from the tree, and sending the
  # checkout as context would be pointless traffic over the 9p share.
  $ctx = Join-Path ([System.IO.Path]::GetTempPath()) "engine-linux-ctx-$DockerfileHash"
  New-Item -ItemType Directory -Force -Path $ctx | Out-Null
  try {
    & docker build -f $Dockerfile -t $Image $ctx
    if ($LASTEXITCODE -ne 0) { throw "docker build failed ($LASTEXITCODE)" }
  } finally { Remove-Item -Recurse -Force $ctx -ErrorAction SilentlyContinue }
}

function Ensure-Volumes {
  foreach ($v in @($SrcVolume, $DepsVolume)) {
    & docker volume inspect $v 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      Write-Step "creating volume $v"
      & docker volume create $v | Out-Null
    }
  }
}

function Invoke-Sync {
  Write-Step "syncing $Root into $SrcVolume"
  $excludeArgs = ($SyncExcludes | ForEach-Object { "--exclude=$_" }) -join ' '
  # --delete so a file removed on the host disappears in the volume; excluded paths are
  # protected from it by default, which is what keeps /src/build alive across syncs.
  $syncScript = "rsync -a --delete $excludeArgs /host/ /src/ && echo ""sync: `$(find /src -type f -not -path '/src/build/*' | wc -l) files"""
  $syncArgs = @('run', '--rm', '-v', "${HostMount}:/host:ro", '-v', "${SrcVolume}:/src",
                $Image, 'bash', '-lc', $syncScript)
  & docker @syncArgs
  if ($LASTEXITCODE -ne 0) { throw "source sync failed ($LASTEXITCODE)" }
}

function Get-RunArgs {
  @(
    '--rm',
    '-v', "${SrcVolume}:/src",
    '-v', "${DepsVolume}:/deps",
    '-w', '/src',
    # Ninja and CTest both write plain text; without this the odd UTF-8 in the tree's comments
    # comes back mangled in the Windows console.
    '-e', 'LANG=C.UTF-8'
  )
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
    $docsArgs = @('run', '--rm', '-v', "${SrcVolume}:/src", '-v', "${mount}:/gate:ro", '-w', '/src',
                  '-e', 'LANG=C.UTF-8', $Image, 'bash', '-lc',
                  'tools/docs-gate.sh --files-from /gate/files.txt --messages-from /gate/messages.txt && tools/docs-check.sh')
    & docker @docsArgs
    exit $LASTEXITCODE
  } finally { Remove-Item -Recurse -Force $gate -ErrorAction SilentlyContinue }
}

function Invoke-Preset([string]$name) {
  $lines = @()
  $lines += 'set -o pipefail'
  if ($Rebuild) { $lines += "rm -rf /src/build/$name" }
  # FETCHCONTENT_BASE_DIR moves the downloaded dependencies out of build/<preset>/_deps and into
  # the second volume, one directory per preset (the dependencies' *build* directories live
  # there too and a Debug tree cannot share one with a Release tree). -Rebuild then costs a
  # compile and not a 700 MB download, and a warm run never touches the network.
  #
  # FETCHCONTENT_UPDATES_DISCONNECTED stops the git-based dependencies re-running `git fetch` on
  # every reconfigure: the tags are pinned, so a fetch can only cost time and a network.
  $lines += "cmake --preset $name -DFETCHCONTENT_BASE_DIR=/deps/$name -DFETCHCONTENT_UPDATES_DISCONNECTED=ON"
  # -k 0 is ci.yml's: keep going after a failure so one run reports every error in the tree
  # rather than the first file Ninja happened to reach.
  $lines += "cmake --build --preset $name -j $Jobs -- -k 0"
  if ($Test) {
    $testCmd = "ctest --preset $name"
    if ($Filter) { $testCmd += " -R '$Filter'" }
    $lines += $testCmd
  }
  $runScript = $lines -join "`n"

  Write-Step "$name (jobs=$Jobs$(if ($Test) { ', with tests' }))"
  $started = [DateTime]::UtcNow
  $runArgs = @('run') + (Get-RunArgs) + @($Image, 'bash', '-lc', $runScript)
  # The compiler's diagnostics are the product of this script, so they go to the caller's
  # streams as they arrive. The status therefore cannot be this function's return value — that
  # is its whole output stream — and is left in a script-scoped variable instead.
  & docker @runArgs
  $script:PresetStatus = $LASTEXITCODE
  $elapsed = [DateTime]::UtcNow - $started
  $verdict = if ($script:PresetStatus -eq 0) { 'ok' } else { "FAILED ($script:PresetStatus)" }
  $colour = if ($script:PresetStatus -eq 0) { 'Green' } else { 'Red' }
  Write-Host ("-- {0}: {1} in {2:m\m\ ss\s}" -f $name, $verdict, $elapsed) -ForegroundColor $colour
}

function Invoke-Shell {
  Write-Step "shell in $Image ($SrcVolume at /src, $DepsVolume at /deps)"
  # -it only when there is a terminal to attach: docker refuses `-t` with redirected input, and
  # "the input device is not a TTY" is a poor way to learn that this switch wants a console.
  $tty = if ([Console]::IsInputRedirected) { @() } else { @('-it') }
  $runArgs = @('run') + $tty + (Get-RunArgs) + @($Image, 'bash')
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
  Write-Host 'The image is shared between checkouts and is left alone; remove it with'
  Write-Host "  docker image rm $Image"
  exit $failed
}

# --- run ---------------------------------------------------------------------------------------

Ensure-Docker
if ($Prune) { Invoke-Prune }
Ensure-Image
Ensure-Volumes
if ($Sync) { Invoke-Sync }
if ($Docs) { Invoke-DocsChecks }
if ($Shell) { Invoke-Shell }

[string[]]$targets = if ($Preset -eq 'all') { $AllPresets } else { @($Preset) }
$results = [ordered]@{}
$overall = 0
foreach ($p in $targets) {
  $script:PresetStatus = 0
  Invoke-Preset $p
  $results[$p] = $script:PresetStatus
  if ($script:PresetStatus -ne 0 -and $overall -eq 0) { $overall = $script:PresetStatus }
}

if ($targets.Count -gt 1) {
  Write-Host ''
  Write-Step 'summary'
  foreach ($k in $results.Keys) {
    $v = $results[$k]
    Write-Host ("  {0,-24} {1}" -f $k, $(if ($v -eq 0) { 'ok' } else { "FAILED ($v)" })) `
      -ForegroundColor $(if ($v -eq 0) { 'Green' } else { 'Red' })
  }
}

exit $overall
