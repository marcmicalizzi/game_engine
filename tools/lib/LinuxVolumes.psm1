# The Linux container build's per-checkout Docker volumes (tools/linux-build.ps1): their names, the
# labels they are created with, and which of them a sweep removes.
#
# The decision is a pure function of facts the script gathers — the volumes with their labels and
# sizes, the repository's checkouts with their merge state, and the containers holding volumes — so
# tools/linux-build.Tests.ps1 drives every rule without a Docker daemon, and the script's own part is
# only to ask docker and git and print. docs/ci/local-linux.md ("Volumes that outlive their
# checkout") is the long form, with the numbers that made this necessary.

Set-StrictMode -Version Latest

# The only names a sweep may touch: the two a checkout's build creates. The shared dependency cache
# (engine-linux-fetch-cache), buildx's state and every other project's volumes never match.
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
    checkout as $Root spells it; engine.role is src, deps or fetch-cache; engine.scope is checkout
    or shared; engine.created is the UTC creation time.
  #>
  param(
    [Parameter(Mandatory)][ValidateSet('src', 'deps', 'fetch-cache')][string]$Role,
    [string]$Root,
    [DateTime]$Created = [DateTime]::UtcNow
  )
  $labels = [ordered]@{ 'engine.role' = $Role }
  if ($Role -eq 'fetch-cache') {
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

Export-ModuleMember -Function Get-ShortHash, Get-CheckoutVolumeId, Get-CheckoutVolumeName, ConvertTo-CheckoutRoot,
  ConvertTo-RootKey, Get-VolumeLabels, ConvertFrom-WorktreePorcelain, ConvertFrom-ContainerMounts,
  ConvertFrom-DockerSize, Format-DockerSize, New-VolumeFact, New-CheckoutFact, Get-VolumeVerdict,
  Get-OwnVolumeVerdict, Format-VolumeVerdict
