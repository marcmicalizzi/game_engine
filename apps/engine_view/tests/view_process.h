#pragma once

// How engine-view's end-to-end tests start a program (engine-view, engine-content): on this test
// process's own desktop where Windows gives one (tests/support/test_desktop.h, roadmap A40), so
// the windows the interactive, replay and present cases open are on nobody's screen, never take
// the owner's foreground, and behave the same whatever the owner's desktop is doing; an offscreen
// run opens no window and is the same run either way. Where no desktop can be made (another OS,
// or a window station that refuses one) the program runs on this desktop as before, and the first
// such run says why.

#include <core/base/types.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_desktop.h>

// <windows.h>, which test_desktop.h includes, defines `near` and `far` as nothing, and engine
// headers name parameters so (core/math's projections); clang-format sorts this header first in
// its includer, ahead of them.
#if defined(_WIN32)
#undef near
#undef far
#endif

#include <span>
#include <string>
#include <string_view>

namespace engine::view_test {

// Runs `argv` with stdin at its end and stdout and stderr merged into `output`, and returns its
// exit code; fails the test (and returns -1) when it cannot be started. A child that gave up
// waiting for the GPU lock ends this process with 75 either way, so CTest reports a skip.
inline i32 run_program(std::span<const std::string_view> argv, std::string& output) {
  std::string error;
#if defined(_WIN32)
  const test::PrivateDesktop& desktop = test::private_desktop();
  if (desktop.valid()) {
    int code = -1;
    if (!test::run_on_desktop(desktop, argv, output, code, error)) {
      FAIL("cannot start " << argv[0] << " on " << desktop.name() << ": " << error);
      return -1;
    }
    return static_cast<i32>(code);
  }
  static bool said = false;
  if (!said) {
    said = true;
    MESSAGE("no desktop of its own here, so the programs run on this one: " << desktop.error());
  }
#endif
  platform::Process p;
  if (!p.spawn(argv, &error, /*merge_stderr=*/true)) {
    FAIL("cannot spawn " << argv[0] << ": " << error);
    return -1;
  }
  p.close_stdin();
  p.read_all(output);
  return p.wait();
}

}  // namespace engine::view_test
