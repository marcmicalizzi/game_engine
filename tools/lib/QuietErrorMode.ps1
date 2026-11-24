# The error mode every process a script starts inherits (roadmap T218; docs/subsystems/platform.md,
# "No engine binary opens a dialog when it fails"). Dot-source it from any script that starts engine
# binaries, then call Set-QuietErrorMode once before the first one:
#
#     . (Join-Path <repo> 'tools/lib/QuietErrorMode.ps1')
#     $previous = Set-QuietErrorMode
#     ...
#     Restore-ErrorMode $previous
#
# Why a script has to: a binary whose DLL is missing is stopped by the loader before its main, so
# before `platform::quiet_error_dialogs()` can switch the boxes off, and the box it raises waits for a
# click (Sunday 2026-11-22's stood on the owner's desktop into Monday). A child created by
# CreateProcess inherits its parent's error mode unless the parent passes CREATE_DEFAULT_ERROR_MODE
# (Microsoft's SetErrorMode and process-creation-flags pages), so a launcher's mode is meant to
# reach a process before its main does; whether it covers the loader's box for a static import at
# process start is open (T218; docs/subsystems/platform.md). It does not reach a test through
# CTest: a non-interactive CTest (`--interactive-debug-mode 0`, the test presets'
# `execution.interactiveDebugging: false`) replaces its own mode with 0x0003 and creates every test
# with that, and an interactive one creates each with CREATE_DEFAULT_ERROR_MODE (CMake 4.3.1,
# cmCTest.cxx and cmProcess.cxx). What it reaches is what a script starts itself: the benches, the
# listener check's copies, a bundle's tests.
#
# The mode is SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX (0x8003), the
# one `platform::quiet_error_dialogs()` sets in the engine's own processes. Nothing here is a module:
# a script dot-sources it, and tools/lib/NoListeners.psm1 does too. Its pair is the inline copy at
# the head of tools/bundle/run-tests.ps1 (the same 0x8003, since a bundle carries no tools/): change
# one and the other together.

function Initialize-ErrorModeNative {
  if (-not ('EngineErrorMode.Native' -as [type])) {
    Add-Type -Namespace EngineErrorMode -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetErrorMode(uint uMode);
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint GetErrorMode();
'@
  }
}

# The quiet mode, for this process and everything it starts from now on. Returns the mode it
# replaced, for Restore-ErrorMode; on a system with no error modes (not Windows), 0 and nothing done.
function Set-QuietErrorMode {
  if (-not ($IsWindows -or $env:OS -eq 'Windows_NT')) { return [uint32]0 }
  Initialize-ErrorModeNative
  return [EngineErrorMode.Native]::SetErrorMode([uint32]0x8003)
}

function Restore-ErrorMode([uint32]$Mode) {
  if ('EngineErrorMode.Native' -as [type]) { [void][EngineErrorMode.Native]::SetErrorMode($Mode) }
}

# This process's error mode, for a script that wants to say what its children inherit.
function Get-ProcessErrorMode {
  if (-not ($IsWindows -or $env:OS -eq 'Windows_NT')) { return [uint32]0 }
  Initialize-ErrorModeNative
  [EngineErrorMode.Native]::GetErrorMode()
}
