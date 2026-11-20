# The matcher of tools/miscompile-scan.ps1, as functions over a listing's lines and over names, so
# it is tested with no compiler at all (tools/miscompile-scan.Tests.ps1, CTest `tools.miscompile_scan`).
#
# What is looked for (docs/experiments/msvc-ob2-avx2-miscompile.md, "Looking for other instances:
# the listings"): MSVC 19.44 and 19.51 at /O2 can leave the caller's copy of a by-value aggregate
# unwritten. In a listing (`cl /FA`) that is, inside one function, a compiler temporary (`$T<n>`)
# whose address is passed to a call and that nothing in the function writes. On its own that is far
# too common to read (a return slot looks the same), so a unit is compiled twice, as the tree
# compiles it and with the optimizer pass that drops the stores turned off, and only a temporary
# that is address-only in the first listing where the second has none is reported: one whose stores
# that pass removed.

$script:Widths = @{ BYTE = 1; WORD = 2; DWORD = 4; QWORD = 8; XMMWORD = 16; YMMWORD = 32; ZMMWORD = 64; TBYTE = 10 }

# Every name of a general register, to the 64-bit register it is part of.
$script:Registers = @{}
foreach ($r in @(@('rax', 'eax', 'ax', 'al', 'ah'), @('rbx', 'ebx', 'bx', 'bl', 'bh'), @('rcx', 'ecx', 'cx', 'cl', 'ch'),
                 @('rdx', 'edx', 'dx', 'dl', 'dh'), @('rsi', 'esi', 'si', 'sil'), @('rdi', 'edi', 'di', 'dil'),
                 @('rbp', 'ebp', 'bp', 'bpl'))) {
  foreach ($n in $r) { $script:Registers[$n] = $r[0] }
}
foreach ($i in 8..15) { foreach ($suffix in '', 'd', 'w', 'b') { $script:Registers["r$i$suffix"] = "r$i" } }

# The registers a call does not preserve (the x64 convention): a pointer kept in one is gone after it.
$script:Volatile = @('rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11')
# Where an argument's address travels: the four registers, and `stack` for the fifth argument onward
# (the address stored into the outgoing area, [rsp+32] and up).
$script:ArgumentPlaces = @('rcx', 'rdx', 'r8', 'r9', 'stack')

<#
  The address-only temporaries of every function of one listing.

  Returns a hashtable: "<function>`t<place>`t<callee>" -> how many temporaries of that function had
  their address passed to that call in that place (rcx, rdx, r8, r9, or `stack`) and were never
  written. A temporary whose address reaches no call that way is counted under the register that
  took it and an empty callee.

  "Written" counts a store by the temporary's own name, by another name at the same frame offset,
  and through any register that holds a frame address when the store is made: the entry `rsp` kept
  in `rax` or `r11` (`mov rax, rsp`), a frame pointer (`lea rbp, [rsp-K]`), or the address of the
  temporary itself (`lea rcx, $T1[rsp]` then `mov [rcx], ...`). Offsets are kept from the stack
  pointer at entry, so the three ways of naming one slot meet.

  "Passed" is read in the order the listing is written, not the order it runs: the next `call` line
  after the address is taken, unless the register is overwritten first (then it was compared or
  kept, not passed). A jump between the two can pair an address with the wrong call, which is one
  reason a candidate is read and not believed.

  What it cannot see, and so reports rather than hides: a store made by a call (memset, a
  constructor) and a store through a pointer it lost track of.
#>
function Get-AddressOnlyTemps {
  [CmdletBinding()]
  param([Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.IEnumerable[string]]$Lines)
  $out = @{}
  $proc = ''; $defs = @{}
  $refs = @{}; $stores = [System.Collections.Generic.List[object]]::new()
  $pending = @{}   # place -> the uses whose address is in it and has met no call yet
  $alias = @{}; $pushes = 0; $delta = -1; $lastEax = -1
  foreach ($line in $Lines) {
    if ($line.Length -eq 0) { continue }
    $c = $line[0]
    if (-not $proc) {
      if ($c -eq "`t" -or $c -eq ' ' -or $c -eq ';') { continue }
      if ($line -match '^(\S+) = (-?\d+)') { $defs[$Matches[1]] = [int]$Matches[2]; continue }
      if ($line -match '^(\S+)\s+PROC\b') {
        $proc = $Matches[1]; $refs = @{}; $stores.Clear(); $pending = @{}; $alias = @{}; $pushes = 0; $delta = -1; $lastEax = -1
      }
      continue
    }
    if ($c -ne "`t" -and $c -ne ' ') {
      if ($line -match '^\S+\s+ENDP\b') {
        foreach ($k in $refs.Keys) {
          $r = $refs[$k]
          if ($r.lea -eq 0 -or $r.other -ne 0) { continue }
          $written = $false
          if ($null -ne $r.at) {
            foreach ($s in $stores) { if ($s.space -eq $r.space -and $s.at -le $r.at -and $r.at -lt $s.at + $s.w) { $written = $true; break } }
          }
          if ($written) { continue }
          # The use that passes it: the first in an argument's place that met a call.
          $x = $null
          foreach ($u in $r.uses) { if ($u.call -and $u.reg -in $script:ArgumentPlaces) { $x = $u; break } }
          $key = if ($x) { "$proc`t$($x.reg)`t$($x.call)" } else { "$proc`t$($r.uses[0].reg)`t" }
          $out[$key] = 1 + [int]$out[$key]
        }
        $proc = ''; $defs = @{}
      }
      continue   # labels and comments
    }
    if ($line -notmatch '^\s+(\w+)\s*(.*)$') { continue }
    $op = $Matches[1]; $rest = $Matches[2]
    $semi = $rest.IndexOf(';'); if ($semi -ge 0) { $rest = $rest.Substring(0, $semi) }
    $rest = $rest.TrimEnd()

    if ($op -eq 'call') {
      $callee = ($rest -split '\s+')[0]
      # What sits in an argument's place is passed to this call; an address in another register the
      # call does not preserve is gone; one in a register it preserves travels on and may be passed later.
      foreach ($place in @($pending.Keys)) {
        if ($place -in $script:ArgumentPlaces) { foreach ($x in $pending[$place]) { $x.call = $callee }; $pending.Remove($place) }
        elseif ($place -in $script:Volatile) { $pending.Remove($place) }
      }
      foreach ($v in $script:Volatile) { $alias.Remove($v) }
      continue
    }
    # The frame's size, from the prologue: pushes, then `sub rsp, N` (or N in eax through __chkstk).
    if ($delta -lt 0) {
      if ($op -eq 'push') { ++$pushes; continue }
      if ($op -eq 'sub' -and $rest -match '^rsp, (\d+)$') { $delta = 8 * $pushes + [int]$Matches[1]; continue }
      if ($op -eq 'sub' -and $rest -eq 'rsp, rax' -and $lastEax -ge 0) { $delta = 8 * $pushes + $lastEax; continue }
      if ($op -eq 'mov' -and $rest -match '^eax, (\d+)$') { $lastEax = [int]$Matches[1] }
    }
    $comma = $rest.IndexOf(',')
    $dest = if ($comma -ge 0) { $rest.Substring(0, $comma).Trim() } else { $rest.Trim() }
    $src = if ($comma -ge 0) { $rest.Substring($comma + 1).Trim() } else { '' }
    # Where the stack pointer is now, from its value at entry.
    $spNow = if ($delta -ge 0) { -$delta } else { -8 * $pushes }

    $temps = if ($line.Contains('$T')) { [regex]::Matches($line, '\$T\d+\$?(?=\[)') } else { $null }

    if ($dest.Contains('[')) {
      # A store (or a compare against memory, which is taken for one: that errs towards reporting less
      # only for a slot something already names, and such a temporary is not address-only anyway).
      if ($comma -ge 0 -and $op -ne 'cmp' -and $op -ne 'test') {
        $w = 8; $at = $null; $space = 'sp'
        if ($dest -match '^(?:(\w+) PTR )?([\w\$@?]+)\[(rsp|rbp)(?:([-+]\d+))?\]$') {
          if ($Matches[1] -and $script:Widths.ContainsKey($Matches[1])) { $w = $script:Widths[$Matches[1]] }
          $base = $defs[$Matches[2]]
          if ($null -ne $base) {
            $rel = $base + $(if ($Matches[4]) { [int]$Matches[4] } else { 0 })
            if ($Matches[3] -eq 'rsp') { $at = $spNow + $rel }
            elseif ($alias.ContainsKey('rbp')) { $at = $alias['rbp'] + $rel }
            else { $at = $rel; $space = 'bp' }
          }
        } elseif ($dest -match '^(?:(\w+) PTR )?\[(\w+)(?:([-+]\d+))?\]$') {
          if ($Matches[1] -and $script:Widths.ContainsKey($Matches[1])) { $w = $script:Widths[$Matches[1]] }
          $k = if ($Matches[3]) { [int]$Matches[3] } else { 0 }
          $reg = $Matches[2]
          if ($reg -eq 'rsp') {
            $at = $spNow + $k
            # An address stored into the outgoing argument area is an argument passed on the stack.
            $from = $script:Registers[$src]
            if ($k -ge 32 -and $from -and $pending.ContainsKey($from)) {
              foreach ($x in @($pending[$from])) {
                $y = @{ reg = 'stack'; call = ''; k = $x.k; copied = $false }
                $refs[$x.k].uses.Add($y)
                if (-not $pending.ContainsKey('stack')) { $pending['stack'] = [System.Collections.Generic.List[object]]::new() }
                $pending['stack'].Add($y)
              }
            }
          } elseif ($alias.ContainsKey($reg)) { $at = $alias[$reg] + $k }
        }
        if ($null -ne $at) { $stores.Add(@{ space = $space; at = $at; w = $w }) }
      }
      if ($temps) { foreach ($m in $temps) { $k = $m.Value; if (-not $refs.ContainsKey($k)) { $refs[$k] = @{ lea = 0; other = 0; at = $null; space = 'sp'; uses = [System.Collections.Generic.List[object]]::new() } }; $refs[$k].other++ } }
      continue
    }

    $reg = $script:Registers[$dest]
    $pointsAt = $null   # the frame address this instruction leaves in $reg, if it is one
    if ($reg -and $comma -ge 0) {
      if ($op -eq 'mov') {
        if ($src -eq 'rsp') { $pointsAt = $spNow }
        elseif ($alias.ContainsKey($src)) { $pointsAt = $alias[$src] }
      } elseif ($op -eq 'lea') {
        if ($src -match '^(?:\w+ PTR )?\[(\w+)(?:([-+]\d+))?\]$') {
          $k = if ($Matches[2]) { [int]$Matches[2] } else { 0 }
          if ($Matches[1] -eq 'rsp') { $pointsAt = $spNow + $k }
          elseif ($alias.ContainsKey($Matches[1])) { $pointsAt = $alias[$Matches[1]] + $k }
        } elseif ($src -match '^(?:\w+ PTR )?([\w\$@?]+)\[(rsp|rbp)(?:([-+]\d+))?\]$') {
          $base = $defs[$Matches[1]]
          if ($null -ne $base) {
            $rel = $base + $(if ($Matches[3]) { [int]$Matches[3] } else { 0 })
            if ($Matches[2] -eq 'rsp') { $pointsAt = $spNow + $rel }
            elseif ($alias.ContainsKey('rbp')) { $pointsAt = $alias['rbp'] + $rel }
          }
        }
      }
    }
    $writes = $reg -and $op -ne 'cmp' -and $op -ne 'test' -and $op -ne 'push'
    if ($writes -and $pending.Count) {
      # A copy of a pending address travels on in the new register; the old one is overwritten below.
      $from = if ($op -eq 'mov') { $script:Registers[$src] } else { $null }
      $moved = if ($from -and $from -ne $reg -and $pending.ContainsKey($from)) { @($pending[$from]) } else { $null }
      # Overwritten before any call: what was in the register was compared or kept, not passed.
      $pending.Remove($reg)
      if ($moved) {
        $list = [System.Collections.Generic.List[object]]::new()
        foreach ($x in $moved) { $y = @{ reg = $reg; call = ''; k = $x.k }; $refs[$x.k].uses.Add($y); $list.Add($y) }
        $pending[$reg] = $list
      }
    }
    if ($temps) {
      foreach ($m in $temps) {
        $k = $m.Value
        if (-not $refs.ContainsKey($k)) { $refs[$k] = @{ lea = 0; other = 0; at = $null; space = 'sp'; uses = [System.Collections.Generic.List[object]]::new() } }
        if ($op -eq 'lea' -and $reg) {
          $x = @{ reg = $reg; call = ''; k = $k }
          $refs[$k].lea++; $refs[$k].uses.Add($x)
          if (-not $pending.ContainsKey($reg)) { $pending[$reg] = [System.Collections.Generic.List[object]]::new() }
          $pending[$reg].Add($x)
          if ($null -ne $pointsAt) { $refs[$k].at = $pointsAt; $refs[$k].space = 'sp' }
          elseif ($src -match '\[rbp' -and $null -ne $defs[$k]) { $refs[$k].at = $defs[$k]; $refs[$k].space = 'bp' }
        } else { $refs[$k].other++ }
      }
    }
    if ($reg) {
      if ($null -ne $pointsAt) { $alias[$reg] = $pointsAt }
      # Anything else that names the register first overwrites it. Not an epilogue's `pop rbp`: a
      # function with several returns goes on naming its frame by rbp after the first of them.
      elseif ($writes -and -not ($op -eq 'pop' -and $reg -eq 'rbp')) { $alias.Remove($reg) }
    }
  }
  return $out
}

<#
  What a temporary's address at a call is, from the function it is in, the place it travels in and
  the callee's decorated name. Everything but 'candidate' is a temporary the callee fills itself, or
  one that is not an argument, and is set aside; a candidate is something to read.

  The callee's own signature is what follows the last calling-convention code of its name (`YA` or
  `SA` for a free or static function, a letter and `EAA` or `EBA` for a member): a local lambda's
  name carries its enclosing function's signature first, and that one says nothing about this call.
#>
function Get-TempSiteClass {
  [CmdletBinding()]
  param([string]$Function, [string]$Register, [string]$Callee)
  if ($Function -match '^\?(dtor|catch|fin)\$') { return 'funclet' }              # an unwinder's funclet
  if (-not $Callee) { return 'not passed' }                                        # compared or kept, never given to a call
  if ($Register -notin $script:ArgumentPlaces) { return 'not an argument' }
  $marks = [regex]::Matches($Callee, '@(?:(YA|SA)|[A-Z]E[AB]A)')
  if ($marks.Count -eq 0) { return 'candidate' }                                   # an indirect call, or C: nothing to go by
  $last = $marks[$marks.Count - 1]
  $member = -not $last.Groups[1].Success
  $tail = $Callee.Substring($last.Index + $last.Length)                            # the return type, then the parameters
  # A function's encoding that ends before the name does (a function pointer among the parameters,
  # or a parameter of a local type, whose name carries its enclosing function): the last code may
  # not be the callee's own, so nothing is concluded from it.
  if ($tail -cmatch '(@Z|XZ)@') { return 'candidate' }
  # A class returned by value comes back through an address the caller passes first; and a function
  # of no parameters has nothing else for that register to carry. (Not certain in one case, said on
  # the page: a free function may return a plain struct of eight bytes or less in rax, and then rcx
  # is its first argument.)
  $slot = $tail -cmatch '^\?[AB][UVTW]' -or $tail -cmatch 'XZ$'
  # A member whose name encodes no return type (a lambda's operator(), its type deduced) and whose
  # first parameter is a number, passed in a register by value: an address in rdx cannot be that
  # parameter, so it is the result's.
  $numberFirst = $tail -cmatch '^@(?:[CDEFGHIJKMN]|_[JKNW])'
  if ($member) {
    if ($Register -eq 'rcx') { return 'this' }                                     # the object is the callee's to fill
    if ($Register -eq 'rdx' -and ($slot -or $numberFirst)) { return 'return slot' }
  } elseif ($Register -eq 'rcx' -and $slot) { return 'return slot' }
  return 'candidate'
}

<#
  The sites of one unit: the temporaries address-only in the plain listing beyond those of the
  switched one, each with its class. -Plain and -Switched are Get-AddressOnlyTemps' results.
#>
function Compare-AddressOnlyTemps {
  [CmdletBinding()]
  param([Parameter(Mandatory)][hashtable]$Plain, [Parameter(Mandatory)][hashtable]$Switched)
  $sites = [System.Collections.Generic.List[object]]::new()
  foreach ($key in ($Plain.Keys | Sort-Object)) {
    $extra = [int]$Plain[$key] - [int]$Switched[$key]
    if ($extra -le 0) { continue }
    $p = $key -split "`t"
    $sites.Add([pscustomobject]@{
        Function = $p[0]; Register = $p[1]; Callee = $p[2]; Count = $extra
        Class = Get-TempSiteClass -Function $p[0] -Register $p[1] -Callee $p[2]
      })
  }
  return $sites.ToArray()   # enumerated: a caller wraps it in @()
}

<#
  One unit's two compile lines from its entry in compile_commands.json: the compiler launcher
  dropped, the inlining level replaced, a listing asked for with its object beside it
  (`<listing>.obj`: two compiles sharing one object path, or the null device, fail each other when
  units run side by side), warnings not errors, and no debug information or include report written.
  Returns @{ Compiler; PlainArguments; SwitchedArguments } and the two whole lines, Plain and Switched.
#>
function Get-ScanCommands {
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Command, [Parameter(Mandatory)][string]$Level,
        [Parameter(Mandatory)][string]$PlainListing, [Parameter(Mandatory)][string]$SwitchedListing,
        [string]$Switch = '/d2ssa-cse-')
  if ($Level -notmatch '^Ob[0-3]$') { throw "miscompile-scan: a level is Ob0 to Ob3, not '$Level'" }
  $at = $Command.IndexOf('cl.exe', [StringComparison]::OrdinalIgnoreCase)
  if ($at -lt 0) { throw 'miscompile-scan: not an MSVC compile command (no cl.exe in it)' }
  # The compiler's own path starts at the last space before `cl.exe`, or at a quote when it is quoted.
  $start = $Command.LastIndexOf(' ', $at) + 1
  if ($start -gt 0 -and $at -gt 0) {
    $quote = $Command.LastIndexOf('"', $at)
    if ($quote -ge 0 -and (($Command.Substring(0, $quote) -split '"').Count % 2) -eq 1) { $start = $quote }
  }
  $cmd = $Command.Substring($start)
  $cmd = $cmd -creplace '\s[/-]showIncludes\b', ''
  $cmd = $cmd -creplace '\s[/-]Fd("[^"]*"|\S+)', ''
  $cmd = $cmd -creplace '\s[/-]Z[7iI]\b', ''
  $cmd = $cmd -creplace '\s[/-]Ob[0-3]\b', ''
  if ($cmd -cnotmatch '\s[/-]Fo("[^"]*"|\S+)') { throw 'miscompile-scan: the compile command names no object (/Fo)' }
  # `/WX-` after the tree's `/WX`: a warning that only another inlining level raises is not the scan's business.
  $plain = $cmd -creplace '\s[/-]Fo("[^"]*"|\S+)', " /$Level /WX- /Fo`"$PlainListing.obj`" /FA /Fa`"$PlainListing`""
  $switched = $cmd -creplace '\s[/-]Fo("[^"]*"|\S+)', " /$Level $Switch /WX- /Fo`"$SwitchedListing.obj`" /FA /Fa`"$SwitchedListing`""
  # The compiler apart from its arguments: an app's line is past the 8,191 characters a command
  # shell takes, so a caller hands the arguments over in a response file.
  $end = if ($plain.StartsWith('"')) { $plain.IndexOf('"', 1) + 1 } else { $plain.IndexOf(' ') }
  $compiler = $plain.Substring(0, $end).Trim('"')
  return @{ Compiler = $compiler; PlainArguments = $plain.Substring($end).Trim(); SwitchedArguments = $switched.Substring($end).Trim(); Plain = $plain; Switched = $switched }
}

# Whether a file of compile_commands.json is the engine's own code: under the repository, C++, and
# neither vendored nor fetched. -Relative is the path from the repository's root, forward slashes.
function Test-ScanUnit {
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Relative, [string]$Filter = '')
  if ($Relative -notmatch '\.(cpp|cc|cxx)$') { return $false }
  if ($Relative -match '^(third_party/|build/[^/]+/_deps/)' -or $Relative -match '^[A-Za-z]:|^/|^\.\./') { return $false }
  if ($Filter -and $Relative -notmatch $Filter) { return $false }
  return $true
}

<#
  The sites already read: tools/miscompile-scan-read.txt, one line a site,

      <unit, or * for any> <place> <callee, or its start and a *> : <why it is not the fault>

  A candidate on the list is reported as `read` and does not fail the scan. The list is for what a
  name cannot say (an indirect call's return slot, a C function that returns a struct, an address
  paired with the wrong call through a jump) and for the canary's planted instance, which the scan
  must go on finding. A line needs its reason; `#` starts a comment.
#>
function Read-ScanReadList {
  [CmdletBinding()]
  param([Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines)
  $entries = [System.Collections.Generic.List[object]]::new()
  $n = 0
  foreach ($line in $Lines) {
    ++$n
    $text = $line.Trim()
    if (-not $text -or $text.StartsWith('#')) { continue }
    if ($text -notmatch '^(\S+)\s+(\S+)\s+(\S+)\s+:\s+(\S.*)$') { throw "miscompile-scan: line $n of the read list is not '<unit> <place> <callee> : <reason>': $text" }
    $entries.Add([pscustomobject]@{ Unit = $Matches[1]; Place = $Matches[2]; Callee = $Matches[3]; Reason = $Matches[4]; Line = $n; Used = $false })
  }
  return $entries.ToArray()
}

# The entry of the read list a site matches, or $null. A callee ending in * matches by its start.
function Find-ScanReadEntry {
  [CmdletBinding()]
  param([AllowEmptyCollection()][object[]]$Entries, [string]$Unit, [string]$Place, [string]$Callee)
  foreach ($e in $Entries) {
    if (($e.Unit -cne '*' -and $e.Unit -cne $Unit) -or $e.Place -cne $Place) { continue }
    if ($e.Callee.EndsWith('*')) { if ($Callee.StartsWith($e.Callee.Substring(0, $e.Callee.Length - 1), [StringComparison]::Ordinal)) { return $e } }
    elseif ($e.Callee -ceq $Callee) { return $e }
  }
  return $null
}

Export-ModuleMember -Function Get-AddressOnlyTemps, Get-TempSiteClass, Compare-AddressOnlyTemps, Get-ScanCommands, Test-ScanUnit, Read-ScanReadList, Find-ScanReadEntry
