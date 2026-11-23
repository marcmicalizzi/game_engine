#pragma once

// The failed assertion `assert_fail` (core/base/assert.h) is ending the process for (roadmap T216).
// A header of its own, not assert.h's, because every translation unit of the tree includes that
// one and only the fault line (core/platform's fault_report.cpp) reads this.

namespace engine {

// Noted by `assert_fail` before it prints, so the fault line its `abort()` meets (core/platform,
// docs/subsystems/platform.md "Every fatal fault prints one line") names the assertion rather than
// the abort. The pointers are `assert_fail`'s arguments, alive while the failing thread is inside
// it, which is until the process ends. The first failure in the process is the one kept.
struct AssertFailure {
  const char* expression = "";
  const char* message = "";
  const char* file = "";
  int line = 0;
};

// Null before any assertion has failed.
const AssertFailure* failed_assertion() noexcept;

}  // namespace engine
