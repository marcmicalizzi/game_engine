#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/lib/MiscompileScan.psm1, the matcher of tools/miscompile-scan.ps1. Runs under
  CTest as `tools.miscompile_scan`.

.DESCRIPTION
  Canned listings, shaped as `cl /FA` writes them, so no compiler runs and the cases hold on any
  system: the faulted function of docs/experiments/msvc-ob2-avx2-miscompile.md is reported and its
  right compile is not; a store counts by the temporary's name, by another name at the same slot,
  through the entry stack pointer kept in rax or r11, through a frame pointer and through the
  temporary's own address, and stops counting through a register a call has overwritten; a return
  slot, a `this` and an unwinder's funclet are set aside; a unit's compile line loses its launcher,
  its debug information and the tree's object and gains the level, the listing and an object of its own.

      pwsh tools/miscompile-scan.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/MiscompileScan.psm1') -Force
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}
# A listing from text: tabs as cl writes them (an instruction line starts with one).
function Get-Temps([string]$text) {
  $lines = [System.Collections.Generic.List[string]]::new()
  foreach ($l in ($text -split "`r?`n")) { $lines.Add(($l -replace '^    ', "`t")) }
  return Get-AddressOnlyTemps -Lines $lines
}

Write-Host 'miscompile-scan tests'

$noise = '?noise3@detail@rocks@engine@@YAN_KUD3@123@@Z'
# The page's listing, cut to what the matcher reads: $T2 is written and passed, $T3 only passed.
$faulted = @"
_TEXT SEGMENT
`$T2 = 48
`$T3 = 72
tv1181 = 96
?chart_of@@YAXXZ PROC
    mov rax, rsp
    push rbx
    sub rsp, 160
    lea rdx, QWORD PTR `$T2[rsp]
    vdivsd xmm0, xmm8, xmm12
    vmovsd QWORD PTR `$T2[rsp], xmm0
    vmovsd QWORD PTR `$T2[rsp+16], xmm0
    vmovsd QWORD PTR `$T2[rsp+8], xmm1
    call $noise
    mov rcx, QWORD PTR tv1181[rsp]
    lea rdx, QWORD PTR `$T3[rsp]         ; a second copy
    vmovaps xmm6, xmm0
    call $noise
    add rsp, 160
    pop rbx
    ret 0
?chart_of@@YAXXZ ENDP
_TEXT ENDS
"@
$right = $faulted -replace 'vmovaps xmm6, xmm0', "vmovsd QWORD PTR `$T3[rsp], xmm7`n    vmovaps xmm6, xmm0"
$plain = Get-Temps $faulted
$switched = Get-Temps $right
Test-That 'the faulted function: one temporary passed in rdx and never written' { $plain.Count -eq 1 -and $plain["?chart_of@@YAXXZ`trdx`t$noise"] -eq 1 }
Test-That 'the same function compiled rightly has none' { $switched.Count -eq 0 }
$sites = @(Compare-AddressOnlyTemps -Plain $plain -Switched $switched)
Test-That 'the difference of the two is one candidate site, by function, register and callee' {
  $sites.Count -eq 1 -and $sites[0].Class -eq 'candidate' -and $sites[0].Function -eq '?chart_of@@YAXXZ' -and $sites[0].Register -eq 'rdx' -and $sites[0].Callee -eq $noise -and $sites[0].Count -eq 1
}
Test-That 'a temporary address-only in both listings is not a site' { @(Compare-AddressOnlyTemps -Plain $plain -Switched $plain).Count -eq 0 }
Test-That 'two in the plain listing against one in the switched is one' {
  $s = @(Compare-AddressOnlyTemps -Plain @{ "f`trdx`tg" = 2 } -Switched @{ "f`trdx`tg" = 1 }); $s.Count -eq 1 -and $s[0].Count -eq 1
}

# One function, the temporary at [rsp+32] of a frame of 8 (a push) + 80: 56 below the entry rsp.
function New-Function([string]$body, [string]$defs = "`$T1 = 32") {
  return "$defs`n?f@@YAXXZ PROC`n    mov rax, rsp`n    push rdi`n    sub rsp, 80`n$body`n    lea rdx, QWORD PTR `$T1[rsp]`n    call ?g@@YAXXZ`n    ret 0`n?f@@YAXXZ ENDP"
}
Test-That 'no store at all is reported' { (Get-Temps (New-Function '    xor ecx, ecx')).Count -eq 1 }
Test-That 'a store through rax holding the entry rsp is a write' { (Get-Temps (New-Function '    mov QWORD PTR [rax-56], rcx')).Count -eq 0 }
Test-That 'a store through rax at another slot is not' { (Get-Temps (New-Function '    mov QWORD PTR [rax-24], rcx')).Count -eq 1 }
Test-That 'a wide store that covers the slot is a write' { (Get-Temps (New-Function '    vmovdqu YMMWORD PTR [rax-80], ymm0')).Count -eq 0 }
Test-That 'a store through rax after a call is not (the call overwrote rax)' { (Get-Temps (New-Function "    call ?h@@YAXXZ`n    mov QWORD PTR [rax-56], rcx")).Count -eq 1 }
Test-That 'a store through rax after rax was loaded is not' { (Get-Temps (New-Function "    mov rax, QWORD PTR [rcx]`n    mov QWORD PTR [rax-56], rcx")).Count -eq 1 }
Test-That 'a store by another name at the same slot is a write' { (Get-Temps (New-Function '    mov QWORD PTR x$[rsp+8], rcx' "`$T1 = 32`nx`$ = 24")).Count -eq 0 }
Test-That 'a store by the raw stack pointer is a write' { (Get-Temps (New-Function '    mov DWORD PTR [rsp+32], ecx')).Count -eq 0 }
Test-That 'a store through a copy of the frame address is a write' { (Get-Temps (New-Function "    mov r10, rax`n    mov QWORD PTR [r10-56], rcx")).Count -eq 0 }
Test-That 'a store through the temporary''s own address is a write' {
  (Get-Temps "`$T1 = 32`n?f@@YAXXZ PROC`n    sub rsp, 88`n    lea rcx, QWORD PTR `$T1[rsp]`n    mov QWORD PTR [rcx], rax`n    call ?g@@YAXXZ`n    ret 0`n?f@@YAXXZ ENDP").Count -eq 0
}
Test-That 'the entry rsp kept in r11 counts as it does in rax' {
  (Get-Temps "`$T1 = 32`n?f@@YAXXZ PROC`n    mov r11, rsp`n    sub rsp, 88`n    mov QWORD PTR [r11-56], rcx`n    lea rdx, QWORD PTR `$T1[rsp]`n    call ?g@@YAXXZ`n    ret 0`n?f@@YAXXZ ENDP").Count -eq 0
}
# A frame pointer: rbp = entry - 8 - 64; the temporary at rbp - 8 + 16, which is rsp + 104 of a 168-byte frame.
$framed = "`$T1 = 16`n?f@@YAXXZ PROC`n    push rbp`n    lea rbp, QWORD PTR [rsp-64]`n    sub rsp, 160`nSTORE`n    lea rcx, QWORD PTR `$T1[rbp-8]`n    call ?g@@YAXXZ`n    lea rsp, QWORD PTR [rbp+96]`n    pop rbp`n    ret 0`n?f@@YAXXZ ENDP"
Test-That 'a temporary named from a frame pointer, never written, is reported' { (Get-Temps ($framed -replace 'STORE', '    xor eax, eax')).Count -eq 1 }
Test-That 'and a store to its slot by the raw stack pointer is a write' { (Get-Temps ($framed -replace 'STORE', '    mov QWORD PTR [rsp+104], rax')).Count -eq 0 }
Test-That 'and so is one by another name from the frame pointer' { (Get-Temps (("y`$ = 0`n" + $framed) -replace 'STORE', '    mov QWORD PTR y$[rbp+8], rax')).Count -eq 0 }
Test-That 'a temporary read or written by name is not address-only' { (Get-Temps (New-Function '    mov rcx, QWORD PTR $T1[rsp]')).Count -eq 0 }
Test-That 'two functions keep their own names and frames' {
  $t = Get-Temps ((New-Function '    xor ecx, ecx') + "`n" + ((New-Function '    mov QWORD PTR [rax-56], rcx') -replace '\?f@@', '?k@@'))
  $t.Count -eq 1 -and $t.ContainsKey("?f@@YAXXZ`trdx`t?g@@YAXXZ")
}
Test-That 'an address taken and passed to nothing has no callee' { $t = Get-Temps "`$T1 = 32`n?f@@YAXXZ PROC`n    sub rsp, 88`n    lea rax, QWORD PTR `$T1[rsp]`n    ret 0`n?f@@YAXXZ ENDP"; $t["?f@@YAXXZ`trax`t"] -eq 1 }

# How the address travels to the call. One function of an 88-byte frame; BODY takes the address.
function New-Passing([string]$body) { return "`$T1 = 48`nx`$ = 64`n?f@@YAXXZ PROC`n    sub rsp, 88`n$body`n    call ?g@@YAXXZ`n    ret 0`n?f@@YAXXZ ENDP" }
function Get-OnlyKey([string]$body) { $t = Get-Temps (New-Passing $body); if ($t.Count -eq 1) { @($t.Keys)[0] } else { "($($t.Count) keys)" } }
Test-That 'a register overwritten before the call passed nothing: compared or kept' {
  (Get-OnlyKey "    lea rcx, QWORD PTR `$T1[rsp]`n    cmp rax, rcx`n    mov rcx, QWORD PTR [rax]") -eq "?f@@YAXXZ`trcx`t"
}
Test-That 'an address stored into the outgoing area is an argument on the stack' {
  (Get-OnlyKey "    lea rax, QWORD PTR `$T1[rsp]`n    mov QWORD PTR [rsp+40], rax") -eq "?f@@YAXXZ`tstack`t?g@@YAXXZ"
}
Test-That 'an address copied into an argument register is passed there' {
  (Get-OnlyKey "    lea rax, QWORD PTR `$T1[rsp]`n    mov rdx, rax") -eq "?f@@YAXXZ`trdx`t?g@@YAXXZ"
}
Test-That 'an address kept in a preserved register over one call and passed to the next is paired with the next' {
  (Get-OnlyKey "    lea rbx, QWORD PTR `$T1[rsp]`n    call ?h@@YAXXZ`n    mov rcx, rbx") -eq "?f@@YAXXZ`trcx`t?g@@YAXXZ"
}
Test-That 'an address in a register a call does not preserve does not outlive it' {
  (Get-OnlyKey "    lea rax, QWORD PTR `$T1[rsp]`n    call ?h@@YAXXZ`n    mov rcx, rax") -eq "?f@@YAXXZ`trax`t"
}
Test-That 'taken twice, it is counted where it is passed, not where it was first taken' {
  (Get-OnlyKey "    lea rax, QWORD PTR `$T1[rsp]`n    mov QWORD PTR x`$[rsp], rax`n    lea rdx, QWORD PTR `$T1[rsp]") -eq "?f@@YAXXZ`trdx`t?g@@YAXXZ"
}

Test-That 'a free function returning a class takes its result in rcx: a return slot' { (Get-TempSiteClass '?f@@YAXXZ' 'rcx' '?make@@YA?AUChartUv@@N@Z') -eq 'return slot' }
Test-That 'a member returning a class takes it in rdx' { (Get-TempSiteClass '?f@@YAXXZ' 'rdx' '??R<lambda_1>@@QEBA?AUChartUv@@II@Z') -eq 'return slot' }
Test-That 'the same member''s rcx is its this' { (Get-TempSiteClass '?f@@YAXXZ' 'rcx' '??R<lambda_1>@@QEBA?AUChartUv@@II@Z') -eq 'this' }
Test-That 'a constructor''s rcx is its this' { (Get-TempSiteClass '?f@@YAXXZ' 'rcx' '??0Mesh@@QEAA@XZ') -eq 'this' }
Test-That 'a by-value argument in rdx of a function returning a double is a candidate' { (Get-TempSiteClass '?f@@YAXXZ' 'rdx' $noise) -eq 'candidate' }
Test-That 'a by-value argument in rcx of a free function returning a double is a candidate' { (Get-TempSiteClass '?f@@YAXXZ' 'rcx' '?length@@YANUD3@@@Z') -eq 'candidate' }
Test-That 'an indirect call cannot be classed and is a candidate' { (Get-TempSiteClass '?f@@YAXXZ' 'rdx' 'QWORD') -eq 'candidate' }
Test-That 'an unwinder''s funclet is set aside' { (Get-TempSiteClass '?dtor$3@?0??f@@YAXXZ@4HA' 'rcx' '?g@@YAXXZ') -eq 'funclet' }
Test-That 'a register that carries no argument is set aside' { (Get-TempSiteClass '?f@@YAXXZ' 'rax' '?g@@YAXXZ') -eq 'not an argument' }
Test-That 'an address no call was given is set aside' { (Get-TempSiteClass '?f@@YAXXZ' 'rdx' '') -eq 'not passed' }
Test-That 'an argument on the stack is a candidate' { (Get-TempSiteClass '?f@@YAXXZ' 'stack' $noise) -eq 'candidate' }
$local = '??R<lambda_1>@?1??DOCTEST_ANON_FUNC_2@@YAXXZ@QEBA@XZ'
Test-That 'a local lambda of no parameters: rdx can only be its result, rcx its this' { (Get-TempSiteClass '?f@@YAXXZ' 'rdx' $local) -eq 'return slot' -and (Get-TempSiteClass '?f@@YAXXZ' 'rcx' $local) -eq 'this' }
Test-That 'a local lambda is read by its own signature, not its enclosing function''s' {
  # The enclosing function returns a class; the lambda returns a double and takes a struct by value.
  (Get-TempSiteClass '?f@@YAXXZ' 'rdx' '??R<lambda_1>@?1??make@@YA?AUChartUv@@XZ@QEBANUD3@@@Z') -eq 'candidate'
}
Test-That 'a lambda taking numbers: rdx cannot be one of them, so it is the result' {
  (Get-TempSiteClass '?f@@YAXXZ' 'rdx' '??R<lambda_1>@?1??DOCTEST_ANON_FUNC_91@@YAXXZ@QEBA@HH@Z') -eq 'return slot' -and (Get-TempSiteClass '?f@@YAXXZ' 'rdx' '??R<lambda_2>@?1??make_block@@YA?AUBlock@@_N@Z@QEBA@NN@Z') -eq 'return slot'
}
Test-That 'a lambda taking a struct first: rdx may be that struct, a candidate' { (Get-TempSiteClass '?f@@YAXXZ' 'rdx' '??R<lambda_1>@?1??g@@YAXXZ@QEBA@UD3@@@Z') -eq 'candidate' }
Test-That 'a function pointer among the parameters leaves nothing concluded' { (Get-TempSiteClass '?f@@YAXXZ' 'rcx' '?each@@YA?AUStats@@P6AXH@Z@Z') -eq 'candidate' }

$line = 'C:\tools\ccache.exe base_dir=X:/w hash_dir=false C:\PROGRA~1\VS\bin\cl.exe  /nologo /TP -DX=1 -Icore /O2 /DNDEBUG -std:c++20 -MD -Z7 /Ob2 /arch:AVX2 /showIncludes /Focore\a.dir\a.cpp.obj /Fdcore\a.dir\a.pdb /FS -c X:\w\core\a.cpp'
$c = Get-ScanCommands -Command $line -Level 'Ob1' -PlainListing 'L\1.plain.asm' -SwitchedListing 'L\1.switched.asm'
Test-That 'the launcher is dropped: the line starts at the compiler' { $c.Plain.StartsWith('C:\PROGRA~1\VS\bin\cl.exe ') -and $c.Compiler -eq 'C:\PROGRA~1\VS\bin\cl.exe' }
Test-That 'the arguments stand apart from the compiler, for a response file' { $c.PlainArguments.StartsWith('/nologo ') -and $c.PlainArguments.EndsWith('-c X:\w\core\a.cpp') -and $c.SwitchedArguments -match ' /d2ssa-cse- ' }
Test-That 'warnings are not errors in a scan compile, after the line''s own /WX' { (Get-ScanCommands -Command ($line -replace ' /O2 ', ' /O2 /WX ') -Level 'Ob1' -PlainListing a -SwitchedListing b).Plain -match ' /WX .* /WX- ' }
Test-That 'the level asked replaces the line''s own' { $c.Plain -match ' /Ob1 ' -and $c.Plain -notmatch '/Ob2' }
Test-That 'the object goes beside the listing, each compile its own' { $c.Plain -match ' /Fo"L\\1\.plain\.asm\.obj" ' -and $c.Switched -match ' /Fo"L\\1\.switched\.asm\.obj" ' }
Test-That 'the tree''s object, its debug information and its include report are not asked for' { $c.Plain -notmatch 'a\.cpp\.obj|/Fd|-Z7|showIncludes' }
Test-That 'the listing is asked for by its path' { $c.Plain -match '/FA /Fa"L\\1\.plain\.asm"' -and $c.Switched -match '/FA /Fa"L\\1\.switched\.asm"' }
Test-That 'only the switched line carries the switch' { $c.Switched -match ' /d2ssa-cse- ' -and $c.Plain -notmatch 'd2ssa' }
Test-That 'the rest of the line is kept' { $c.Plain -match '/arch:AVX2' -and $c.Plain -match '-DX=1' -and $c.Plain.EndsWith('-c X:\w\core\a.cpp') }
Test-That 'a level that is not one is refused' { try { Get-ScanCommands -Command $line -Level 'O2' -PlainListing a -SwitchedListing b | Out-Null; $false } catch { $true } }
Test-That 'a line with no cl.exe is refused' { try { Get-ScanCommands -Command 'clang++ -c a.cpp -o a.o' -Level 'Ob1' -PlainListing a -SwitchedListing b | Out-Null; $false } catch { $true } }

Test-That 'a library source is a unit' { Test-ScanUnit -Relative 'domain/rocks/src/cliff.cpp' }
Test-That 'so are a test, a bench, an app and a generated source' {
  (Test-ScanUnit -Relative 'domain/rocks/tests/cliff_tests.cpp') -and (Test-ScanUnit -Relative 'core/math/bench/math_bench.cpp') -and
  (Test-ScanUnit -Relative 'apps/engine_view/main.cpp') -and (Test-ScanUnit -Relative 'build/msvc-release/generated/schemas/doc.gen.cpp')
}
Test-That 'vendored and fetched code is not' { -not (Test-ScanUnit -Relative 'third_party/doctest/main.cpp') -and -not (Test-ScanUnit -Relative 'build/msvc-release/_deps/jolt-src/Jolt/Core/Memory.cpp') }
Test-That 'a file outside the repository is not' { -not (Test-ScanUnit -Relative 'C:/fetch/flecs/flecs.cpp') -and -not (Test-ScanUnit -Relative '../x/a.cpp') }
Test-That 'C is not (every C target is third party)' { -not (Test-ScanUnit -Relative 'core/base/src/a.c') }
Test-That 'a filter narrows the units' { (Test-ScanUnit -Relative 'domain/rocks/src/cliff.cpp' -Filter '^domain/rocks/') -and -not (Test-ScanUnit -Relative 'domain/doc/src/document.cpp' -Filter '^domain/rocks/') }

$list = @(Read-ScanReadList -Lines @('# a comment', '', 'a/b.cpp rcx ecs_cpp_set : a C function returning a struct: rcx is its result', 'a/c.cpp rdx ?noise3@* : the canary''s planted instance'))
Test-That 'the read list holds a line a site, comments and blank lines left out' { $list.Count -eq 2 -and $list[0].Unit -eq 'a/b.cpp' -and $list[0].Place -eq 'rcx' -and $list[0].Callee -eq 'ecs_cpp_set' -and $list[0].Reason -match '^a C function' -and $list[1].Line -eq 4 }
Test-That 'a site is found by unit, place and callee' { (Find-ScanReadEntry -Entries $list -Unit 'a/b.cpp' -Place 'rcx' -Callee 'ecs_cpp_set').Line -eq 3 }
Test-That 'another place, another unit or another callee is not on the list' {
  $null -eq (Find-ScanReadEntry -Entries $list -Unit 'a/b.cpp' -Place 'rdx' -Callee 'ecs_cpp_set') -and $null -eq (Find-ScanReadEntry -Entries $list -Unit 'a/z.cpp' -Place 'rcx' -Callee 'ecs_cpp_set') -and
  $null -eq (Find-ScanReadEntry -Entries $list -Unit 'a/b.cpp' -Place 'rcx' -Callee 'ecs_cpp_set2')
}
Test-That 'a callee ending in * matches by its start' { (Find-ScanReadEntry -Entries $list -Unit 'a/c.cpp' -Place 'rdx' -Callee $noise).Line -eq 4 -and $null -eq (Find-ScanReadEntry -Entries $list -Unit 'a/c.cpp' -Place 'rdx' -Callee '?other@@YANXZ') }
Test-That 'a unit of * stands for any unit' { $any = @(Read-ScanReadList -Lines @('* rcx ecs_cpp_set : a C function returning a struct')); (Find-ScanReadEntry -Entries $any -Unit 'x/y.cpp' -Place 'rcx' -Callee 'ecs_cpp_set').Line -eq 1 -and $null -eq (Find-ScanReadEntry -Entries $any -Unit 'x/y.cpp' -Place 'rdx' -Callee 'ecs_cpp_set') }
Test-That 'a line with no reason is refused' { try { Read-ScanReadList -Lines @('a/b.cpp rcx ecs_cpp_set') | Out-Null; $false } catch { $true } }
Test-That 'an empty list finds nothing' { $null -eq (Find-ScanReadEntry -Entries @() -Unit 'a/b.cpp' -Place 'rcx' -Callee 'x') }

if ($failures.Count -gt 0) {
  Write-Host "miscompile-scan: $($failures.Count) of $checks checks FAILED" -ForegroundColor Red
  exit 1
}
Write-Host "miscompile-scan: $checks checks passed" -ForegroundColor Green
exit 0
