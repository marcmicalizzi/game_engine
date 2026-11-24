// engine_platform_error_mode_probe: CTest test `platform.error_mode` (roadmap T218;
// docs/subsystems/platform.md, "No engine binary opens a dialog when it fails").
//
// It prints the error mode its process was created with and exits 0 only when that mode holds
// SEM_FAILCRITICALERRORS and SEM_NOGPFAULTERRORBOX (0x0003): the mode a test must be created with,
// because a binary the loader stops for a missing DLL never reaches the
// `platform::quiet_error_dialogs()` that sets the same mode from inside. What it holds is the test
// presets' `execution.interactiveDebugging: false`, the change that mends T218's incident: under a
// non-interactive CTest the test is created with CTest's own mode, and under an interactive one
// (CTest's default outside a dashboard) with the default mode, 0, and the case fails.
//
// Where its 0x0003 comes from (CMake 4.3.1's source, tag v4.3.1): `cmCTest::ExecuteTests` calls
// `BlockTestErrorDiagnostics()` when CTest is not in interactive debug mode, and on Windows that
// routine calls `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` on CTest itself,
// replacing whatever mode CTest inherited, and puts DART_TEST_FROM_DART=1 and
// DASHBOARD_TEST_FROM_CTEST into the environment every test inherits (Source/cmCTest.cxx);
// `cmProcess` then passes UV_PROCESS_WINDOWS_USE_PARENT_ERROR_MODE, again only when not
// interactive (Source/CTest/cmProcess.cxx), without which its bundled libuv creates the test with
// CREATE_DEFAULT_ERROR_MODE (Utilities/cmlibuv/src/win/process.c). So the 0x0003 measured on
// 2026-11-23 through `tools/dev.ps1 test` is CTest's mode, not the launcher's: the 0x8003
// `tools/dev.ps1` sets never reaches a test through `ctest`, and this probe cannot tell whether
// `dev.ps1` set anything. `dev.ps1`'s own call covers what it starts directly (the benches) and
// what a shell whose mode is not already quiet would otherwise pass on.
//
// It links no engine module on purpose, and so calls no `require_cpu_baseline()`: that function
// runs before main in every binary that links `core/platform` and sets the very mode this probe
// reads, which would make the reading the engine's and not its creator's. It runs nothing but
// one system call and a print, so it carries no instruction the baseline check would guard.
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

int main() {
#if defined(_WIN32)
  const UINT mode = ::GetErrorMode();
  const UINT wanted = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX;
  std::printf("error mode inherited: 0x%04X (wanted 0x%04X)\n", mode, wanted);
  if ((mode & wanted) != wanted) {
    std::printf(
        "the process that started this test passes no quiet error mode on: run tests through "
        "tools/dev.ps1 test or a non-interactive ctest (--interactive-debug-mode 0)\n");
    return 1;
  }
#endif
  return 0;
}
