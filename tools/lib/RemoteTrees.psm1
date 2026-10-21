# Which of the GPU server's checkout trees `tools/remote-build.ps1 -Prune -Stale` removes. The facts
# are gathered by the script (one ssh listing, `git worktree list`, the disk); the decision is a pure
# function of them, so tools/linux-build.Tests.ps1 can write a server down and check every verdict
# without one. docs/ci/remote-linux.md, "Trees that outlive their checkout".
#
# A tree is ~/game_engine-remote/<id>, where <id> is twelve hex digits of SHA-256 over the local
# checkout's path, lower-cased (the containers' volume id, tools/lib/LinuxVolumes.psm1). The hash
# cannot be reversed, so since batch 33 every run also writes <id>/checkout.txt naming the checkout
# (`root=`) and the machine (`host=`) it came from; a tree from before then has no marker and is
# recognised only by hashing the checkouts this machine has.
#
# The rule, and why each part is there:
#   - A tree touched in the last hour is kept, whatever else is true: a build is writing it, and a
#     sweep never races one. The age is the newest of the tree, its src/ and its tmp/ (every run
#     rewrites tmp/ccache.before before it compiles).
#   - A marker naming another machine is kept: whether its checkout exists is that machine's to say.
#   - A marker naming this machine is stale when that checkout is gone from disk, and kept when it
#     is there.
#   - No marker: kept when its id is the id of a checkout of this repository that is on disk;
#     otherwise stale, because nothing names its checkout — unless -KeepUnknown, for a server that
#     another clone or another machine also builds on from before the marker existed.
# Only what is under ~/game_engine-remote/ and named by twelve hex digits is ever a tree; the
# compiler cache (~/game_engine-ccache) and anything else in the home directory are not looked at.

Set-StrictMode -Version Latest

$script:BusySeconds = 3600

function ConvertFrom-RemoteTreeListing {
  <#
  .SYNOPSIS
    The listing script's lines (`T|<id>|<bytes>|<age seconds>|<marker root>|<marker host>`) as
    objects: Id, Bytes ($null when du gave nothing), AgeSeconds ($null likewise), Root, Host.
  #>
  param([AllowEmptyCollection()] [AllowEmptyString()] [string[]]$Lines = @())
  $out = @()
  foreach ($line in $Lines) {
    $f = "$line" -split '\|', 6
    if ($f.Count -lt 6 -or $f[0] -ne 'T' -or $f[1] -cnotmatch '^[0-9a-f]{12}$') { continue }
    $bytes = if ($f[2] -match '^\d+$') { [double]$f[2] } else { $null }
    $age = if ($f[3] -match '^-?\d+$') { [long]$f[3] } else { $null }
    $out += [pscustomobject]@{ Id = $f[1]; Bytes = $bytes; AgeSeconds = $age; Root = $f[4].Trim(); Host = $f[5].Trim() }
  }
  return , $out
}

function Get-RemoteTreeVerdict {
  <#
  .SYNOPSIS
    One verdict per tree: Id, Bytes, Root (what names its checkout, or ''), Stale (bool), Why.
  .PARAMETER Checkouts
    This machine's checkouts of the repository that are on disk, as paths in $Root's spelling.
  .PARAMETER Present
    Whether a path is a checkout on disk (a scriptblock), for a marker's root that no longer is.
  #>
  param(
    [AllowEmptyCollection()] [object[]]$Trees = @(),
    [AllowEmptyCollection()] [string[]]$Checkouts = @(),
    [Parameter(Mandatory)] [string]$ThisHost,
    [Parameter(Mandatory)] [scriptblock]$Present,
    [switch]$KeepUnknown
  )
  $ids = @{}
  foreach ($c in $Checkouts) { if ($c) { $ids[(Get-RemoteTreeId $c)] = $c } }
  $verdicts = @()
  foreach ($t in $Trees) {
    $v = { param($stale, $why, $root) [pscustomobject]@{ Id = $t.Id; Bytes = $t.Bytes; Root = "$root"; Stale = $stale; Why = $why } }
    if ($null -eq $t.AgeSeconds -or $t.AgeSeconds -lt $script:BusySeconds) {
      $verdicts += & $v $false 'touched in the last hour: a build may be using it' $(if ($t.Root) { $t.Root } elseif ($ids.ContainsKey($t.Id)) { $ids[$t.Id] })
      continue
    }
    if ($t.Root) {
      if ($t.Host -and $t.Host -ne $ThisHost) { $verdicts += & $v $false "another machine's checkout ($($t.Host))" $t.Root; continue }
      if (& $Present $t.Root) { $verdicts += & $v $false 'its checkout is on disk' $t.Root }
      else { $verdicts += & $v $true 'its checkout is gone from disk' $t.Root }
      continue
    }
    if ($ids.ContainsKey($t.Id)) { $verdicts += & $v $false 'its checkout is on disk (no marker; matched by id)' $ids[$t.Id]; continue }
    if ($KeepUnknown) { $verdicts += & $v $false 'nothing names its checkout; kept (-KeepUnknown)' ''; continue }
    $verdicts += & $v $true 'nothing names its checkout (no marker, and its id is no checkout on disk)' ''
  }
  return , $verdicts
}

function Get-RemoteTreeId([string]$Root) {
  # remote-build.ps1's own key: the checkout's path as Resolve-Path spells it, lower-cased.
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($Root.ToLowerInvariant())
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try { return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant().Substring(0, 12) }
  finally { $sha.Dispose() }
}

function Get-RemoteTreeListingScript {
  # POSIX sh for the server: one line per tree, nothing else. du's byte count is the tree's real
  # size, build trees and downloaded dependencies included.
  return @'
cd "$HOME/game_engine-remote" 2>/dev/null || exit 0
now=$(date +%s)
for d in */; do
  d=${d%/}
  case "$d" in ????????????) ;; *) continue ;; esac
  case "$d" in *[!0-9a-f]*) continue ;; esac
  bytes=$(du -sb "$d" 2>/dev/null | cut -f1)
  newest=$(stat -c %Y "$d" "$d/src" "$d/tmp" "$d"/tmp/* 2>/dev/null | sort -n | tail -n 1)
  age=""; [ -n "$newest" ] && age=$((now - newest))
  root=""; host=""
  if [ -f "$d/checkout.txt" ]; then
    root=$(sed -n 's/^root=//p' "$d/checkout.txt" | head -n 1)
    host=$(sed -n 's/^host=//p' "$d/checkout.txt" | head -n 1)
  fi
  printf 'T|%s|%s|%s|%s|%s\n' "$d" "$bytes" "$age" "$root" "$host"
done
'@
}

Export-ModuleMember -Function ConvertFrom-RemoteTreeListing, Get-RemoteTreeVerdict, Get-RemoteTreeId, Get-RemoteTreeListingScript
