// engine_platform_error_mode_probe: CTest test `platform.error_mode` (roadmap T218;
// docs/subsystems/platform.md, "No engine binary opens a dialog when it fails").
//
// It prints the error mode its process was created with and exits 0 only when that mode holds
// SEM_FAILCRITICALERRORS and SEM_NOGPFAULTERRORBOX (0x0003): the mode a test must
// inherit from whatever started it, because a binary the loader stops for a missing DLL never
// reaches the `platform::quiet_error_dialogs()` that sets the same mode from inside. So it holds
// the launcher to it — `tools/dev.ps1 test` sets the mode, and the test presets run CTest
// non-interactive, without which CTest (4.0 on) creates every test with the default mode.
//
// It links no engine module on purpose, and so calls no `require_cpu_baseline()`: that function
// runs before main in every binary that links `core/platform` and sets the very mode this probe
// reads, which would make the reading the engine's and not the launcher's. It runs nothing but
// one system call and a print, so it carries no instruction the baseline check would guard.
//
// 0x0003 and not the 0x8003 the launcher sets: measured on 2026-11-23 through tools/dev.ps1 and a
// non-interactive CTest 4.3.1, the child is created with 0x0003, SEM_NOOPENFILEERRORBOX not passed
// on (Microsoft's pages say a child inherits "the error mode" and do not say this bit stays
// behind). That bit is OpenFile's box, not the loader's, so nothing here needs it.
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
