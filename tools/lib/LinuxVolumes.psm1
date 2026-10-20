# The Linux container build's per-checkout Docker volumes (tools/linux-build.ps1): their names, the
# labels they are created with, and which of them a sweep removes.
#
# The decision is a pure function of facts the script gathers — the volumes with their labels and
# sizes, the repository's checkouts with their merge state, and the containers holding volumes — so
# tools/linux-build.Tests.ps1 drives every rule without a Docker daemon, and the script's own part is
# only to ask docker and git and print. docs/ci/local-linux.md ("Volumes that outlive their
# checkout") is the long form, with the numbers that made this necessary.

Set-StrictMode -Version Latest

# The only names a sweep may touch: the two a checkout's build creates. The two machine-wide volumes
# (engine-linux-fetch-cache, engine-linux-ccache), buildx's state and every other project's volumes
# never match.
$script:CheckoutVolumePattern = '^engine-linux-(src|deps)-([0-9a-f]{12})$'

function Get-ShortHash([string]$text) {
  # Twelve hex digits of SHA-256: the image tag's Dockerfile hash and a checkout's volume id.
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try { return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant().Substring(0, 12) }
  finally { $sha.Dispose() }
}

function Get-CheckoutVolumeId([string]$Root) {
  # The checkout's path, lower-cased, is its identity: every worktree is a separate tree and must
  # not share a build directory with another. This is the id every volume has been named with since
  # the script existed; tools/linux-build.Tests.ps1 pins it, because an unlabelled volume is found
  # by nothing else.
  return Get-ShortHash $Root.ToLowerInvariant()
}

function Get-CheckoutVolumeName {
  param([Parameter(Mandatory)][ValidateSet('src', 'deps')][string]$Role, [Parameter(Mandatory)][string]$Root)
  return "engine-linux-$Role-$(Get-CheckoutVolumeId $Root)"
}

function ConvertTo-CheckoutRoot([string]$Path) {
  # A path as linux-build.ps1's $Root spells it — full, with the platform's separators and none at
  # the end — from git's spelling (`D:/workspace/...` on Windows). The volume id hashes that exact
  # spelling, so an unlabelled volume is matched only through this.
  $full = [System.IO.Path]::GetFullPath($Path)
  $trimmed = $full.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
  if ($trimmed -match '^[A-Za-z]:$' -or -not $trimmed) { return $full }
  return $trimmed
}

function ConvertTo-RootKey([string]$Root) {
  # How two spellings of one checkout are compared: blind to case and separators, no trailing one.
  # The id already ignores case, so two roots that differ only in case are one checkout here too.
  if (-not $Root) { return '' }
  return ($Root -replace '\\', '/').TrimEnd('/').ToLowerInvariant()
}

function Get-VolumeLabels {
  <#
  .SYNOPSIS
    The labels linux-build.ps1 creates a volume with, so that a sweep names a volume's checkout
    from the volume alone instead of hashing every checkout it can find. engine.root is the
    checkout as $Root spells it; engine.role is src, deps, fetch-cache or ccache; engine.scope is
    checkout, or shared for the two machine-wide volumes; engine.created is the UTC creation time.
  #>
  param(
    [Parameter(Mandatory)][ValidateSet('src', 'deps', 'fetch-cache', 'ccache')][string]$Role,
    [string]$Root,
    [DateTime]$Created = [DateTime]::UtcNow
  )
  $labels = [ordered]@{ 'engine.role' = $Role }
  if ($Role -in @('fetch-cache', 'ccache')) {
    $labels['engine.scope'] = 'shared'
  } else {
    if (-not $Root) { throw "a $Role volume is per checkout and needs its root" }
    $labels['engine.scope'] = 'checkout'
    $labels['engine.root'] = $Root
  }
  $labels['engine.created'] = $Created.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", [System.Globalization.CultureInfo]::InvariantCulture)
  return $labels
}

function ConvertFrom-WorktreePorcelain {
  <#
  .SYNOPSIS
    `git worktree list --porcelain`, as one object per checkout: Path (git's spelling), Head,
    Branch (short name, or $null when detached), Bare, Locked and LockReason, Prunable, and IsMain
    — git lists the main checkout first.
  #>
  param([AllowEmptyString()][AllowEmptyCollection()][string[]]$Lines = @())
  $records = New-Object System.Collections.Generic.List[object]
  $current = $null
  foreach ($raw in @($Lines) + @('')) {
    $line = "$raw".TrimEnd("`r")
    if (-not $line) {
      if ($current) { $records.Add([pscustomobject]$current); $current = $null }
      continue
    }
    $word, $rest = $line -split ' ', 2
    if ($word -eq 'worktree') {
      if ($current) { $records.Add([pscustomobject]$current) }
      $current = [ordered]@{ Path = $rest; Head = $null; Branch = $null; Bare = $false; Locked = $false
                             LockReason = $null; Prunable = $false; IsMain = ($records.Count -eq 0) }
      continue
    }
    if (-not $current) { continue }
    switch ($word) {
      'HEAD' { $current.Head = $rest }
      'branch' { $current.Branch = ($rest -replace '^refs/heads/', '') }
      'bare' { $current.Bare = $true }
      'locked' { $current.Locked = $true; $current.LockReason = $rest }
      'prunable' { $current.Prunable = $true }
    }
  }
  return , $records.ToArray()
}

function ConvertFrom-ContainerMounts {
  <#
  .SYNOPSIS
    `docker ps -a --no-trunc --format '{{.Names}}|{{.State}}|{{.Mounts}}'`, as a table from volume
    name to the containers holding it ("name (state)"). docker refuses to remove a volume any
    container holds, running or stopped, so a sweep names the container instead of asking.
  #>
  param([AllowEmptyString()][AllowEmptyCollection()][string[]]$Lines = @())
  $holders = @{}
  foreach ($line in @($Lines)) {
    $f = "$line" -split '\|', 3
    if ($f.Count -lt 3) { continue }
    foreach ($mount in ($f[2] -split ',')) {
      $m = $mount.Trim()
      if (-not $m) { continue }
      $who = "$($f[0]) ($($f[1]))"
      $holders[$m] = if ($holders.ContainsKey($m)) { "$($holders[$m]), $who" } else { $who }
    }
  }
  return $holders
}

function ConvertFrom-DockerSize([string]$Text) {
  # docker's sizes are decimal (go-units HumanSize): "0B", "35.4kB", "218MB", "1.608GB". $null
  # for anything else ("N/A" while a size is still being computed).
  if ("$Text" -notmatch '^\s*([0-9]+(?:\.[0-9]+)?)\s*([kKMGTP]?)B\s*$') { return $null }
  # A hashtable's keys ignore case, so 'k' answers for docker's "kB" and for a "KB".
  $scale = @{ '' = 1; 'k' = 1e3; 'M' = 1e6; 'G' = 1e9; 'T' = 1e12; 'P' = 1e15 }[$Matches[2]]
  return [double]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture) * $scale
}

function Format-DockerSize([double]$Bytes) {
  # The same decimal units docker prints, so a total reads like the lines above it.
  $units = @('B', 'kB', 'MB', 'GB', 'TB', 'PB')
  $i = 0
  while ($Bytes -ge 1000 -and $i -lt $units.Count - 1) { $Bytes /= 1000; $i++ }
  $format = if ($i -eq 0) { '{0:0}{1}' } else { '{0:0.##}{1}' }
  return [string]::Format([System.Globalization.CultureInfo]::InvariantCulture, $format, $Bytes, $units[$i])
}

function New-VolumeFact {
  param([Parameter(Mandatory)][string]$Name, [System.Collections.IDictionary]$Labels, [string]$Size)
  return [pscustomobject]@{ Name = $Name; Labels = $Labels; Size = $Size }
}

function New-CheckoutFact {
  <#
  .SYNOPSIS
    One checkout, as the decision sees it. Root is spelt as linux-build.ps1's $Root would be.
    Listed: this repository's `git worktree list` names it (false for a root only a label names).
    Present: a checkout is still there — the directory holds a .git. Merged: its HEAD is an
    ancestor of the base, or (MergedByPatch) every commit it has that the base does not is in the
    base by patch, as a rebase lands them (`git cherry`). Locked: `git worktree lock` — the
    harness locks a live agent's worktree. Dirty: uncommitted changes.
  #>
  param(
    [Parameter(Mandatory)][string]$Root,
    [switch]$IsMain,
    [bool]$Listed = $true,
    [bool]$Present = $true,
    [string]$Branch,
    [string]$Head,
    [bool]$Merged = $false,
    [bool]$MergedByPatch = $false,
    [bool]$Locked = $false,
    [string]$LockReason,
    [bool]$Dirty = $false
  )
  return [pscustomobject]@{
    Root = $Root; IsMain = [bool]$IsMain; Listed = $Listed; Present = $Present; Branch = $Branch
    Head = $Head; Merged = $Merged; MergedByPatch = $MergedByPatch; Locked = $Locked
    LockReason = $LockReason; Dirty = $Dirty
  }
}

function Get-VolumeVerdict {
  <#
  .SYNOPSIS
    Which checkout volumes a sweep removes, and why each one stays or goes.

  .DESCRIPTION
    One verdict per volume named engine-linux-src-<id> or engine-linux-deps-<id>, and none for any
    other volume. A volume's checkout is its engine.root label, or, for a volume made before the
    labels, the checkout whose id it carries. In this order, a volume is

      kept    while any container holds it (docker would refuse; the container is named);
      stale   when no checkout's id is its own and it has no label — an "unknown checkout", which no
              build from a checkout this repository lists can mount again — unless -KeepUnknown;
      stale   when its checkout is no longer there;
      kept    for the main checkout, and for the checkout running the sweep;
      kept    for a checkout this repository does not list (another clone: not ours to judge);
      kept    for a locked worktree (the harness locks a live agent's);
      kept    for a worktree whose HEAD has commits the base does not, or with uncommitted changes;
      stale   for a worktree whose HEAD the base contains, or whose every own commit the base
              holds by patch (a rebase merge).

    Every input is data, so the tests can write any machine down.
  #>
  [CmdletBinding()]
  param(
    [AllowEmptyCollection()][object[]]$Volumes = @(),
    [AllowEmptyCollection()][object[]]$Checkouts = @(),
    [System.Collections.IDictionary]$Holders = @{},
    [string]$CurrentRoot = '',
    [string]$Base = 'main',
    [switch]$KeepUnknown
  )
  $byKey = @{}
  $byId = @{}
  foreach ($c in @($Checkouts)) {
    $key = ConvertTo-RootKey $c.Root
    if (-not $byKey.ContainsKey($key)) { $byKey[$key] = $c }
    # The id hashes the spelling the script saw; on Windows that has backslashes, and a caller that
    # passed git's forward slashes should still find the volume.
    foreach ($spelling in @($c.Root, ($c.Root -replace '/', '\'))) {
      $id = Get-CheckoutVolumeId $spelling
      if (-not $byId.ContainsKey($id)) { $byId[$id] = $c }
    }
  }
  $currentKey = ConvertTo-RootKey $CurrentRoot

  $verdicts = New-Object System.Collections.Generic.List[object]
  foreach ($v in @($Volumes)) {
    if ("$($v.Name)" -notmatch $script:CheckoutVolumePattern) { continue }
    $role = $Matches[1]
    $id = $Matches[2]
    $labels = $v.Labels
    $labelRoot = if ($labels -and $labels.Contains('engine.root')) { "$($labels['engine.root'])" } else { '' }

    $checkout = $null
    $root = $null
    if ($labelRoot) {
      $root = $labelRoot
      $checkout = $byKey[(ConvertTo-RootKey $labelRoot)]
    } elseif ($byId.ContainsKey($id)) {
      $checkout = $byId[$id]
      $root = $checkout.Root
    }
    $holder = if ($Holders -and $Holders.Contains($v.Name)) { $Holders[$v.Name] } else { $null }
    $what = 'HEAD'
    if ($checkout) {
      $head = "$($checkout.Head)"
      if ($checkout.Branch) { $what = "branch $($checkout.Branch)" }
      elseif ($head) { $what = "detached HEAD $($head.Substring(0, [Math]::Min(8, $head.Length)))" }
    }

    $stale = $false
    if ($holder) {
      $reason = "in use by $holder"
    } elseif (-not $root) {
      $reason = 'unknown checkout: no label, and no checkout this repository lists has its id'
      if ($KeepUnknown) { $reason += '; kept (-KeepUnknown)' } else { $stale = $true }
    } elseif (-not $checkout) {
      # A caller that gathered facts for every root a label names never gets here; a volume whose
      # checkout was not looked at is kept rather than guessed about.
      $reason = 'its checkout was not examined'
    } elseif (-not $checkout.Present) {
      $stale = $true
      $reason = 'no checkout there any more'
    } elseif ($checkout.IsMain) {
      $reason = 'the main checkout'
    } elseif ($currentKey -and (ConvertTo-RootKey $root) -eq $currentKey) {
      $reason = 'the checkout running this sweep; -Prune without -Stale removes its own'
    } elseif (-not $checkout.Listed) {
      $reason = 'a checkout this repository does not list as a worktree'
    } elseif ($checkout.Locked) {
      $reason = "the worktree is locked$(if ($checkout.LockReason) { " ($($checkout.LockReason))" })"
    } elseif (-not $checkout.Merged) {
      $reason = "$what has commits $Base does not"
    } else {
      $merged = if ($checkout.MergedByPatch) { "is in $Base as rebased commits (git cherry)" } else { "is merged into $Base" }
      if ($checkout.Dirty) {
        $reason = "$what $merged, but the worktree has uncommitted changes"
      } else {
        $stale = $true
        $reason = "$what $merged"
      }
    }

    $verdicts.Add([pscustomobject]@{
        Name = $v.Name; Role = $role; Id = $id; Root = $root; Labelled = [bool]$labelRoot
        Size = $v.Size; Bytes = (ConvertFrom-DockerSize $v.Size); Stale = $stale; Reason = $reason
      })
  }
  return , $verdicts.ToArray()
}

function Get-OwnVolumeVerdict {
  # -Prune without -Stale: this checkout's two volumes, whatever their labels say, unless a
  # container holds one.
  param(
    [AllowEmptyCollection()][object[]]$Volumes = @(),
    [Parameter(Mandatory)][string]$Root,
    [System.Collections.IDictionary]$Holders = @{}
  )
  $mine = @((Get-CheckoutVolumeName -Role src -Root $Root), (Get-CheckoutVolumeName -Role deps -Root $Root))
  $verdicts = New-Object System.Collections.Generic.List[object]
  foreach ($v in @($Volumes)) {
    if ($v.Name -notin $mine -or "$($v.Name)" -notmatch $script:CheckoutVolumePattern) { continue }
    $holder = if ($Holders -and $Holders.Contains($v.Name)) { $Holders[$v.Name] } else { $null }
    $verdicts.Add([pscustomobject]@{
        Name = $v.Name; Role = $Matches[1]; Id = $Matches[2]; Root = $Root
        Labelled = [bool]($v.Labels -and $v.Labels.Contains('engine.root')); Size = $v.Size
        Bytes = (ConvertFrom-DockerSize $v.Size); Stale = (-not $holder)
        Reason = $(if ($holder) { "in use by $holder" } else { "this checkout's own" })
      })
  }
  return , $verdicts.ToArray()
}

function Format-VolumeVerdict {
  # One line per volume: what happens to it, its name, its size, its checkout, and why.
  param([Parameter(Mandatory)][object]$Verdict, [string]$Action)
  if (-not $Action) { $Action = if ($Verdict.Stale) { 'stale' } else { 'keep' } }
  $size = if ($null -ne $Verdict.Bytes) { Format-DockerSize $Verdict.Bytes } else { '?' }
  $root = if ($Verdict.Root) { $Verdict.Root } else { 'unknown checkout' }
  return ('{0,-6} {1,-30} {2,9}  {3}  -- {4}' -f $Action, $Verdict.Name, $size, $root, $Verdict.Reason)
}

# --- the build container: its mounts, its environment, and the script it runs --------------------
#
# Here rather than in linux-build.ps1 so that tools/linux-build.Tests.ps1 checks what a build
# container is given — every volume at its mount point, the compiler cache's settings, the configure
# line's launcher, and the order of build, statistics and exit — without a daemon.

# The two volumes every checkout on the machine shares, at the mount points the image creates for
# them (tools/ci/linux.Dockerfile). The fetch cache holds the dependencies' downloaded sources, one
# directory per pin; the compiler cache holds ccache's results, keyed by what the compiler is given.
# Neither belongs to a checkout, so neither is ever a sweep's (CheckoutVolumePattern above).
$script:SharedVolumes = [ordered]@{
  'fetch-cache' = [pscustomobject]@{ Name = 'engine-linux-fetch-cache'; Mount = '/fetch' }
  'ccache' = [pscustomobject]@{ Name = 'engine-linux-ccache'; Mount = '/ccache' }
}

# The compiler cache's limit. docs/ci/local-linux.md ("The compiler cache") has the measurement it
# comes from: what one compile of each preset adds to the cache, and how many of main's generations
# and checkouts' changes the limit then holds. ccache evicts the least recently used results past
# it; nothing else ever removes anything from the volume.
$script:CcacheMaxSize = '30G'

function Get-SharedVolume {
  param([Parameter(Mandatory)][ValidateSet('fetch-cache', 'ccache')][string]$Role)
  return $script:SharedVolumes[$Role]
}

function Get-CcacheMaxSize { return $script:CcacheMaxSize }

function Get-BuildContainerArgs {
  <#
  .SYNOPSIS
    The `docker run` arguments a checkout's build and shell containers share: its two volumes and
    the machine's two, the working directory, the locale, the compiler cache's settings, and
    --network none with -Offline. -NoCcache (ENGINE_CCACHE=0) leaves the launcher on the configure
    line — so turning the cache off and on again never reconfigures or rebuilds the tree — and makes
    ccache run the compiler directly (CCACHE_DISABLE), which is also how an object compiled through
    the cache is checked against one compiled without it.
  #>
  param(
    [Parameter(Mandatory)][string]$SrcVolume,
    [Parameter(Mandatory)][string]$DepsVolume,
    [Parameter(Mandatory)][string]$RoleLabel,
    [Parameter(Mandatory)][string]$Role,
    [switch]$Offline,
    [switch]$NoCcache
  )
  $fetch = Get-SharedVolume fetch-cache
  $cc = Get-SharedVolume ccache
  $runArgs = @(
    '--rm',
    '--label', "$RoleLabel=$Role",
    '-v', "${SrcVolume}:/src",
    '-v', "${DepsVolume}:/deps",
    '-v', "$($fetch.Name):$($fetch.Mount)",
    '-v', "$($cc.Name):$($cc.Mount)",
    '-w', '/src',
    # Ninja and CTest both write plain text; without this the odd UTF-8 in the tree's comments
    # comes back mangled in the Windows console.
    '-e', 'LANG=C.UTF-8',
    '-e', "CCACHE_DIR=$($cc.Mount)",
    '-e', "CCACHE_MAXSIZE=$script:CcacheMaxSize"
  )
  if ($NoCcache) { $runArgs += @('-e', 'CCACHE_DISABLE=1') }
  if ($Offline) { $runArgs += @('--network', 'none') }
  return $runArgs
}

# The two lines of compiler-cache statistics a build prints. The counters are read before and after
# the build (`ccache --print-stats`, tab-separated) and the difference is printed, rather than
# zeroing them first: the counters are the machine's, and zeroing them would erase what another
# checkout's run is in the middle of counting. The second line is the cache's size against its limit.
$script:CcacheReport = @'
ccache_snapshot() { ccache --print-stats 2>/dev/null > "$1" || : > "$1"; }
ccache_report() {
  if [ -n "${CCACHE_DISABLE:-}" ]; then echo "ccache: disabled for this run (ENGINE_CCACHE=0): every object was compiled"; return 0; fi
  awk -F'\t' '
    NR == FNR { before[$1] = $2; next }
    { d = $2 - before[$1]
      if ($1 == "direct_cache_hit") direct = d
      else if ($1 == "preprocessed_cache_hit") pre = d
      else if ($1 == "cache_miss") miss = d
      else if ($1 !~ /^(direct_cache_miss|preprocessed_cache_miss|cache_size_kibibyte|files_in_cache|cleanups_performed|recache|stats_.*|local_storage_.*|remote_storage_.*)$/ && d > 0) other += d }
    END { hits = direct + pre; total = hits + miss
          if (total == 0 && other == 0) { print "ccache: nothing was compiled"; exit }
          printf "ccache: %d of %d compiles from the cache (%.1f%%: %d direct, %d preprocessed), %d compiled, %d not cacheable\n",
                 hits, total, total ? 100 * hits / total : 0, direct, pre, miss, other }' "$1" "$2" || true
  ccache -s 2>/dev/null | grep -m1 'Cache size' | sed -e 's/^ *//' -e 's/  */ /g' -e "s|^|ccache: $CCACHE_DIR, |" || true
}
'@

function Get-PresetScript {
  <#
  .SYNOPSIS
    The bash script one preset's build container runs: configure with the dependency cache and the
    compiler cache's launcher, build, two lines of cache statistics, then the tests. `set -e`, and
    the build's status is kept across the statistics, so a failed build still ends the run with its
    own status and the tests never run over it (docs/ci/local-linux.md, "The command").
  #>
  param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Jobs = 8,
    [switch]$Test,
    [string]$Filter,
    [switch]$Rebuild,
    [Parameter(Mandatory)][string]$FetchCacheArgs
  )
  $lines = @()
  # -e: a configure or build that fails ends the run with its own status. Without it the script
  # went on to `ctest`, which ran the *previous* build's binaries, passed, and made the container's
  # exit status 0: a -Test run printed "ok" over a compile error (2026-09-23, docs/ci/local-linux.md).
  $lines += 'set -eo pipefail'
  $lines += $script:CcacheReport
  if ($Rebuild) { $lines += "rm -rf /src/build/$Name" }
  # FETCHCONTENT_UPDATES_DISCONNECTED stops a git-based dependency re-running `git fetch` on a
  # reconfigure should one ever be populated the ordinary way (a configure without the cache).
  #
  # The launcher sends every compile through ccache, the engine's and every FetchContent
  # dependency's alike: the two variables initialise every target's <LANG>_COMPILER_LAUNCHER, and
  # the dependencies are targets of this one project. Every checkout's container sees its sources
  # at /src, its dependencies' builds at /deps/<preset> and their sources at /fetch, so a compile
  # command is byte-identical in every checkout and needs no base_dir: an unchanged file is a hit
  # wherever it was last compiled, and the object is the one the compiler made there.
  #
  # CMAKE_DISABLE_PRECOMPILE_HEADERS: the engine uses no precompiled headers, but SDL3
  # (SDL_ENABLE_PCH) and Jolt do, and ccache will not cache a compile that reads one unless told to
  # be sloppy about it. The first runs here counted it: 749 of 2359 compiles "could not use
  # precompiled header", every SDL3 and Jolt source, compiled again in every checkout. Without the
  # header the same sources are ordinary compiles and cache like the rest (docs/ci/local-linux.md,
  # "The compiler cache").
  $lines += "cmake --preset $Name -DFETCHCONTENT_BASE_DIR=/deps/$Name -DFETCHCONTENT_UPDATES_DISCONNECTED=ON $FetchCacheArgs -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON"
  $lines += 'ccache_snapshot /tmp/ccache.before'
  # -k 0 is ci.yml's: keep going after a failure so one run reports every error in the tree
  # rather than the first file Ninja happened to reach.
  $lines += 'build_status=0'
  $lines += "cmake --build --preset $Name -j $Jobs -- -k 0 || build_status=`$?"
  $lines += 'ccache_snapshot /tmp/ccache.after'
  $lines += 'ccache_report /tmp/ccache.before /tmp/ccache.after'
  $lines += 'if [ "$build_status" -ne 0 ]; then exit "$build_status"; fi'
  if ($Test) {
    $testCmd = "ctest --preset $Name"
    if ($Filter) { $testCmd += " -R '$Filter'" }
    $lines += $testCmd
  }
  return ($lines -join "`n")
}

Export-ModuleMember -Function Get-ShortHash, Get-CheckoutVolumeId, Get-CheckoutVolumeName, ConvertTo-CheckoutRoot,
  ConvertTo-RootKey, Get-VolumeLabels, ConvertFrom-WorktreePorcelain, ConvertFrom-ContainerMounts,
  ConvertFrom-DockerSize, Format-DockerSize, New-VolumeFact, New-CheckoutFact, Get-VolumeVerdict,
  Get-OwnVolumeVerdict, Format-VolumeVerdict, Get-SharedVolume, Get-CcacheMaxSize, Get-BuildContainerArgs,
  Get-PresetScript
